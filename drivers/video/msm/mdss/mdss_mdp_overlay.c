/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/msm_mdp.h>
#include <linux/sw_sync.h>

#include <mach/iommu_domains.h>
#include <mach/event_timer.h>

#include "mdss.h"
#include "mdss_debug.h"
#include "mdss_fb.h"
#include "mdss_mdp.h"
#include "mdss_mdp_rotator.h"

#define VSYNC_PERIOD 16
#define BORDERFILL_NDX	0x0BF000BF
#define CHECK_BOUNDS(offset, size, max_size) \
	(((size) > (max_size)) || ((offset) > ((max_size) - (size))))

static atomic_t ov_active_panels = ATOMIC_INIT(0);
static int mdss_mdp_overlay_free_fb_pipe(struct msm_fb_data_type *mfd);
static int mdss_mdp_overlay_fb_parse_dt(struct msm_fb_data_type *mfd);
static int mdss_mdp_overlay_off(struct msm_fb_data_type *mfd);
static void __vsync_retire_signal(struct msm_fb_data_type *mfd, int val);

static int mdss_mdp_overlay_get(struct msm_fb_data_type *mfd,
				struct mdp_overlay *req)
{
	struct mdss_mdp_pipe *pipe;
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);

	pipe = mdss_mdp_pipe_get(mdata, req->id);
	if (IS_ERR_OR_NULL(pipe)) {
		pr_err("invalid pipe ndx=%x\n", req->id);
		return pipe ? PTR_ERR(pipe) : -ENODEV;
	}

	*req = pipe->req_data;
	mdss_mdp_pipe_unmap(pipe);

	return 0;
}

static int mdss_mdp_overlay_req_check(struct msm_fb_data_type *mfd,
				      struct mdp_overlay *req,
				      struct mdss_mdp_format_params *fmt)
{
	u32 xres, yres;
	u32 min_src_size, min_dst_size;
	int content_secure;
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);

	xres = mfd->fbi->var.xres;
	yres = mfd->fbi->var.yres;

	content_secure = (req->flags & MDP_SECURE_OVERLAY_SESSION);
	if (!ctl->is_secure && content_secure &&
				 (mfd->panel.type == WRITEBACK_PANEL)) {
		pr_debug("return due to security concerns\n");
		return -EPERM;
	}
	if (mdata->mdp_rev >= MDSS_MDP_HW_REV_102) {
		min_src_size = fmt->is_yuv ? 2 : 1;
		min_dst_size = 1;
	} else {
		min_src_size = fmt->is_yuv ? 10 : 5;
		min_dst_size = 2;
	}

	if (req->z_order >= MDSS_MDP_MAX_STAGE) {
		pr_err("zorder %d out of range\n", req->z_order);
		return -ERANGE;
	}

	if (req->src.width > MAX_IMG_WIDTH ||
	    req->src.height > MAX_IMG_HEIGHT ||
	    req->src_rect.w < min_src_size || req->src_rect.h < min_src_size ||
	    CHECK_BOUNDS(req->src_rect.x, req->src_rect.w, req->src.width) ||
	    CHECK_BOUNDS(req->src_rect.y, req->src_rect.h, req->src.height)) {
		pr_err("invalid source image img wh=%dx%d rect=%d,%d,%d,%d\n",
		       req->src.width, req->src.height,
		       req->src_rect.x, req->src_rect.y,
		       req->src_rect.w, req->src_rect.h);
		return -EOVERFLOW;
	}

	if (req->dst_rect.w < min_dst_size || req->dst_rect.h < min_dst_size) {
		pr_err("invalid destination resolution (%dx%d)",
		       req->dst_rect.w, req->dst_rect.h);
		return -EOVERFLOW;
	}

	if (req->horz_deci || req->vert_deci) {
		if (!mdata->has_decimation) {
			pr_err("No Decimation in MDP V=%x\n", mdata->mdp_rev);
			return -EINVAL;
		} else if ((req->horz_deci > MAX_DECIMATION) ||
				(req->vert_deci > MAX_DECIMATION))  {
			pr_err("Invalid decimation factors horz=%d vert=%d\n",
					req->horz_deci, req->vert_deci);
			return -EINVAL;
		} else if (req->flags & MDP_BWC_EN) {
			pr_err("Decimation can't be enabled with BWC\n");
			return -EINVAL;
		}
	}

	if (!(req->flags & MDSS_MDP_ROT_ONLY)) {
		u32 src_w, src_h, dst_w, dst_h;

		if ((CHECK_BOUNDS(req->dst_rect.x, req->dst_rect.w, xres) ||
		     CHECK_BOUNDS(req->dst_rect.y, req->dst_rect.h, yres))) {
			pr_err("invalid destination rect=%d,%d,%d,%d\n",
			       req->dst_rect.x, req->dst_rect.y,
			       req->dst_rect.w, req->dst_rect.h);
			return -EOVERFLOW;
		}

		if (req->flags & MDP_ROT_90) {
			dst_h = req->dst_rect.w;
			dst_w = req->dst_rect.h;
		} else {
			dst_w = req->dst_rect.w;
			dst_h = req->dst_rect.h;
		}

		src_w = req->src_rect.w >> req->horz_deci;
		src_h = req->src_rect.h >> req->vert_deci;

		if (src_w > MAX_MIXER_WIDTH) {
			pr_err("invalid source width=%d HDec=%d\n",
					req->src_rect.w, req->horz_deci);
			return -EINVAL;
		}

		if ((src_w * MAX_UPSCALE_RATIO) < dst_w) {
			pr_err("too much upscaling Width %d->%d\n",
			       req->src_rect.w, req->dst_rect.w);
			return -EINVAL;
		}

		if ((src_h * MAX_UPSCALE_RATIO) < dst_h) {
			pr_err("too much upscaling. Height %d->%d\n",
			       req->src_rect.h, req->dst_rect.h);
			return -EINVAL;
		}

		if (src_w > (dst_w * MAX_DOWNSCALE_RATIO)) {
			pr_err("too much downscaling. Width %d->%d H Dec=%d\n",
			       src_w, req->dst_rect.w, req->horz_deci);
			return -EINVAL;
		}

		if (src_h > (dst_h * MAX_DOWNSCALE_RATIO)) {
			pr_err("too much downscaling. Height %d->%d V Dec=%d\n",
			       src_h, req->dst_rect.h, req->vert_deci);
			return -EINVAL;
		}

		if (req->flags & MDP_BWC_EN) {
			if ((req->src.width != req->src_rect.w) ||
			    (req->src.height != req->src_rect.h)) {
				pr_err("BWC: mismatch of src img=%dx%d rect=%dx%d\n",
					req->src.width, req->src.height,
					req->src_rect.w, req->src_rect.h);
				return -EINVAL;
			}

			if ((req->flags & MDP_DECIMATION_EN) ||
					req->vert_deci || req->horz_deci) {
				pr_err("Can't enable BWC and decimation\n");
				return -EINVAL;
			}
		}

		if (req->flags & MDP_DEINTERLACE) {
			if (req->flags & MDP_SOURCE_ROTATED_90) {
				if ((req->src_rect.w % 4) != 0) {
					pr_err("interlaced rect not h/4\n");
					return -EINVAL;
				}
			} else if ((req->src_rect.h % 4) != 0) {
				pr_err("interlaced rect not h/4\n");
				return -EINVAL;
			}
		}
	} else {
		if (req->flags & MDP_DEINTERLACE) {
			if ((req->src_rect.h % 4) != 0) {
				pr_err("interlaced rect h not multiple of 4\n");
				return -EINVAL;
			}
		}
	}

	if (fmt->is_yuv) {
		if ((req->src_rect.x & 0x1) || (req->src_rect.y & 0x1) ||
		    (req->src_rect.w & 0x1) || (req->src_rect.h & 0x1)) {
			pr_err("invalid odd src resolution or coordinates\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int mdss_mdp_overlay_rotator_setup(struct msm_fb_data_type *mfd,
					  struct mdp_overlay *req)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_rotator_session *rot;
	struct mdss_mdp_format_params *fmt;
	int ret = 0;
	u32 bwc_enabled;

	pr_debug("rot ctl=%u req id=%x\n", mdp5_data->ctl->num, req->id);

	fmt = mdss_mdp_get_format_params(req->src.format);
	if (!fmt) {
		pr_err("invalid rot format %d\n", req->src.format);
		return -EINVAL;
	}

	ret = mdss_mdp_overlay_req_check(mfd, req, fmt);
	if (ret)
		return ret;

	if (req->id == MSMFB_NEW_REQUEST) {
		rot = mdss_mdp_rotator_session_alloc();
		rot->pid = current->tgid;
		list_add(&rot->list, &mdp5_data->rot_proc_list);

		if (!rot) {
			pr_err("unable to allocate rotator session\n");
			return -ENOMEM;
		}
	} else if (req->id & MDSS_MDP_ROT_SESSION_MASK) {
		rot = mdss_mdp_rotator_session_get(req->id);

		if (!rot) {
			pr_err("rotator session=%x not found\n", req->id);
			return -ENODEV;
		}
	} else {
		pr_err("invalid rotator session id=%x\n", req->id);
		return -EINVAL;
	}

	/* keep only flags of interest to rotator */
	rot->flags = req->flags & (MDP_ROT_90 | MDP_FLIP_LR | MDP_FLIP_UD |
				   MDP_SECURE_OVERLAY_SESSION);

	bwc_enabled = req->flags & MDP_BWC_EN;
	if (bwc_enabled  &&  !mdp5_data->mdata->has_bwc) {
		pr_err("BWC is not supported in MDP version %x\n",
			mdp5_data->mdata->mdp_rev);
		rot->bwc_mode = 0;
	} else {
		rot->bwc_mode = bwc_enabled ? 1 : 0;
	}
	rot->format = fmt->format;
	rot->img_width = req->src.width;
	rot->img_height = req->src.height;
	rot->src_rect.x = req->src_rect.x;
	rot->src_rect.y = req->src_rect.y;
	rot->src_rect.w = req->src_rect.w;
	rot->src_rect.h = req->src_rect.h;

	if (req->flags & MDP_DEINTERLACE) {
		rot->flags |= MDP_DEINTERLACE;
		rot->src_rect.h /= 2;
	}

	ret = mdss_mdp_rotator_setup(rot);
	if (ret == 0) {
		req->id = rot->session_id;
	} else {
		pr_err("Unable to setup rotator session\n");
		mdss_mdp_rotator_release(rot);
	}

	return ret;
}

static int __mdp_pipe_tune_perf(struct mdss_mdp_pipe *pipe)
{
	struct mdss_data_type *mdata = pipe->mixer->ctl->mdata;
	struct mdss_mdp_perf_params perf;
	int rc;

	for (;;) {
		rc = mdss_mdp_perf_calc_pipe(pipe, &perf);

		if (!rc && (perf.mdp_clk_rate <= mdata->max_mdp_clk_rate))
			break;

		/*
		 * if decimation is available try to reduce minimum clock rate
		 * requirement by applying vertical decimation and reduce
		 * mdp clock requirement
		 */
		if (mdata->has_decimation && (pipe->vert_deci < MAX_DECIMATION)
			&& !pipe->bwc_mode)
			pipe->vert_deci++;
		else
			return -EPERM;
	}

	return 0;
}

static int __mdss_mdp_overlay_setup_scaling(struct mdss_mdp_pipe *pipe)
{
	u32 src;
	int rc;

	src = pipe->src.w >> pipe->horz_deci;
	rc = mdss_mdp_calc_phase_step(src, pipe->dst.w, &pipe->phase_step_x);
	if (rc) {
		pr_err("Horizontal scaling calculation failed=%d! %d->%d\n",
				rc, src, pipe->dst.w);
		return rc;
	}

	src = pipe->src.h >> pipe->vert_deci;
	rc = mdss_mdp_calc_phase_step(src, pipe->dst.h, &pipe->phase_step_y);
	if (rc) {
		pr_err("Vertical scaling calculation failed=%d! %d->%d\n",
				rc, src, pipe->dst.h);
		return rc;
	}

	return 0;
}

static int mdss_mdp_overlay_pipe_setup(struct msm_fb_data_type *mfd,
				       struct mdp_overlay *req,
				       struct mdss_mdp_pipe **ppipe)
{
	struct mdss_mdp_format_params *fmt;
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_mixer *mixer = NULL;
	u32 pipe_type, mixer_mux, len;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdp_histogram_start_req hist;
	int ret;
	u32 bwc_enabled;

	if (mdp5_data->ctl == NULL)
		return -ENODEV;

	if (req->flags & MDP_ROT_90) {
		pr_err("unsupported inline rotation\n");
		return -ENOTSUPP;
	}

	if ((req->dst_rect.w > MAX_DST_W) || (req->dst_rect.h > MAX_DST_H)) {
		pr_err("exceeded max mixer supported resolution %dx%d\n",
				req->dst_rect.w, req->dst_rect.h);
		return -EOVERFLOW;
	}

	if (req->flags & MDSS_MDP_RIGHT_MIXER)
		mixer_mux = MDSS_MDP_MIXER_MUX_RIGHT;
	else
		mixer_mux = MDSS_MDP_MIXER_MUX_LEFT;

	pr_debug("pipe ctl=%u req id=%x mux=%d\n", mdp5_data->ctl->num, req->id,
			mixer_mux);

	if ((req->flags & MDP_BWC_EN) || ((req->flags & MDP_SOURCE_ROTATED_90)
		&& (mdata->mdp_rev < MDSS_MDP_HW_REV_102)))
		req->src.format =
			mdss_mdp_get_rotator_dst_format(req->src.format);

	fmt = mdss_mdp_get_format_params(req->src.format);
	if (!fmt) {
		pr_err("invalid pipe format %d\n", req->src.format);
		return -EINVAL;
	}

	ret = mdss_mdp_overlay_req_check(mfd, req, fmt);
	if (ret)
		return ret;

	pipe = mdss_mdp_mixer_stage_pipe(mdp5_data->ctl, mixer_mux,
					req->z_order);
	if (pipe && pipe->ndx != req->id) {
		pr_debug("replacing pnum=%d at stage=%d mux=%d\n",
				pipe->num, req->z_order, mixer_mux);
		mdss_mdp_mixer_pipe_unstage(pipe);
	}

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, mixer_mux);
	if (!mixer) {
		pr_err("unable to get mixer\n");
		return -ENODEV;
	}

	if (req->id == MSMFB_NEW_REQUEST) {
		if (req->flags & MDP_OV_PIPE_FORCE_DMA)
			pipe_type = MDSS_MDP_PIPE_TYPE_DMA;
		else if (fmt->is_yuv || (req->flags & MDP_OV_PIPE_SHARE))
			pipe_type = MDSS_MDP_PIPE_TYPE_VIG;
		else
			pipe_type = MDSS_MDP_PIPE_TYPE_RGB;

		pipe = mdss_mdp_pipe_alloc(mixer, pipe_type);

		/* VIG pipes can also support RGB format */
		if (!pipe && pipe_type == MDSS_MDP_PIPE_TYPE_RGB) {
			pipe_type = MDSS_MDP_PIPE_TYPE_VIG;
			pipe = mdss_mdp_pipe_alloc(mixer, pipe_type);
		}

		if (pipe == NULL) {
			pr_err("error allocating pipe\n");
			return -ENOMEM;
		}

		ret = mdss_mdp_pipe_map(pipe);
		if (ret) {
			pr_err("unable to map pipe=%d\n", pipe->num);
			return ret;
		}

		mutex_lock(&mfd->lock);
		list_add(&pipe->used_list, &mdp5_data->pipes_used);
		mutex_unlock(&mfd->lock);
		pipe->mixer = mixer;
		pipe->mfd = mfd;
		pipe->pid = current->tgid;
		pipe->play_cnt = 0;
	} else {
		pipe = mdss_mdp_pipe_get(mdp5_data->mdata, req->id);
		if (IS_ERR_OR_NULL(pipe)) {
			pr_err("invalid pipe ndx=%x\n", req->id);
			return pipe ? PTR_ERR(pipe) : -ENODEV;
		}

		if (pipe->mixer != mixer) {
			if (!mixer->ctl || (mixer->ctl->mfd != mfd)) {
				pr_err("Can't switch mixer %d->%d pnum %d!\n",
						pipe->mixer->num, mixer->num,
						pipe->num);
				ret = -EINVAL;
				goto exit_fail;
			}
			pr_debug("switching pipe mixer %d->%d pnum %d\n",
					pipe->mixer->num, mixer->num,
					pipe->num);
			mdss_mdp_mixer_pipe_unstage(pipe);
			pipe->mixer = mixer;
		}
	}

	pipe->flags = req->flags;
	bwc_enabled = req->flags & MDP_BWC_EN;
	if (bwc_enabled  &&  !mdp5_data->mdata->has_bwc) {
		pr_err("BWC is not supported in MDP version %x\n",
			mdp5_data->mdata->mdp_rev);
		pipe->bwc_mode = 0;
	} else {
		pipe->bwc_mode = pipe->mixer->rotator_mode ?
			0 : (bwc_enabled ? 1 : 0) ;
	}
	pipe->img_width = req->src.width & 0x3fff;
	pipe->img_height = req->src.height & 0x3fff;
	pipe->src.x = req->src_rect.x;
	pipe->src.y = req->src_rect.y;
	pipe->src.w = req->src_rect.w;
	pipe->src.h = req->src_rect.h;
	pipe->dst.x = req->dst_rect.x;
	pipe->dst.y = req->dst_rect.y;
	pipe->dst.w = req->dst_rect.w;
	pipe->dst.h = req->dst_rect.h;
	pipe->horz_deci = req->horz_deci;
	pipe->vert_deci = req->vert_deci;
	pipe->src_fmt = fmt;

	pipe->mixer_stage = req->z_order;
	pipe->is_fg = req->is_fg;
	pipe->alpha = req->alpha;
	pipe->transp = req->transp_mask;
	pipe->blend_op = req->blend_op;
	if (pipe->blend_op == BLEND_OP_NOT_DEFINED)
		pipe->blend_op = fmt->alpha_enable ?
					BLEND_OP_PREMULTIPLIED :
					BLEND_OP_OPAQUE;

	if (!fmt->alpha_enable && (pipe->blend_op != BLEND_OP_OPAQUE))
		pr_debug("Unintended blend_op %d on layer with no alpha plane\n",
			pipe->blend_op);

	if (fmt->is_yuv && !(pipe->flags & MDP_SOURCE_ROTATED_90)) {
		pipe->overfetch_disable = OVERFETCH_DISABLE_BOTTOM;

		if (!(pipe->flags & MDSS_MDP_DUAL_PIPE) ||
				(pipe->flags & MDSS_MDP_RIGHT_MIXER))
			pipe->overfetch_disable |= OVERFETCH_DISABLE_RIGHT;
		pr_debug("overfetch flags=%x\n", pipe->overfetch_disable);
	} else {
		pipe->overfetch_disable = 0;
	}

	req->id = pipe->ndx;
	pipe->req_data = *req;

	if (pipe->flags & MDP_OVERLAY_PP_CFG_EN) {
		memcpy(&pipe->pp_cfg, &req->overlay_pp_cfg,
					sizeof(struct mdp_overlay_pp_params));
		len = pipe->pp_cfg.igc_cfg.len;
		if ((pipe->pp_cfg.config_ops & MDP_OVERLAY_PP_IGC_CFG) &&
						(len == IGC_LUT_ENTRIES)) {
			ret = copy_from_user(pipe->pp_res.igc_c0_c1,
					pipe->pp_cfg.igc_cfg.c0_c1_data,
					sizeof(uint32_t) * len);
			if (ret) {
				ret = -ENOMEM;
				goto exit_fail;
			}
			ret = copy_from_user(pipe->pp_res.igc_c2,
					pipe->pp_cfg.igc_cfg.c2_data,
					sizeof(uint32_t) * len);
			if (ret) {
				ret = -ENOMEM;
				goto exit_fail;
			}
			pipe->pp_cfg.igc_cfg.c0_c1_data =
							pipe->pp_res.igc_c0_c1;
			pipe->pp_cfg.igc_cfg.c2_data = pipe->pp_res.igc_c2;
		}
		if (pipe->pp_cfg.config_ops & MDP_OVERLAY_PP_HIST_CFG) {
			if (pipe->pp_cfg.hist_cfg.ops & MDP_PP_OPS_ENABLE) {
				hist.block = pipe->pp_cfg.hist_cfg.block;
				hist.frame_cnt =
					pipe->pp_cfg.hist_cfg.frame_cnt;
				hist.bit_mask = pipe->pp_cfg.hist_cfg.bit_mask;
				hist.num_bins = pipe->pp_cfg.hist_cfg.num_bins;
				mdss_mdp_histogram_start(&hist);
			} else if (pipe->pp_cfg.hist_cfg.ops &
							MDP_PP_OPS_DISABLE) {
				mdss_mdp_histogram_stop(
					pipe->pp_cfg.hist_cfg.block);
			}
		}
		len = pipe->pp_cfg.hist_lut_cfg.len;
		if ((pipe->pp_cfg.config_ops & MDP_OVERLAY_PP_HIST_LUT_CFG) &&
						(len == ENHIST_LUT_ENTRIES)) {
			ret = copy_from_user(pipe->pp_res.hist_lut,
					pipe->pp_cfg.hist_lut_cfg.data,
					sizeof(uint32_t) * len);
			if (ret) {
				ret = -ENOMEM;
				goto exit_fail;
			}
			pipe->pp_cfg.hist_lut_cfg.data = pipe->pp_res.hist_lut;
		}
	}

	if (pipe->flags & MDP_DEINTERLACE) {
		if (pipe->flags & MDP_SOURCE_ROTATED_90) {
			pipe->src.w /= 2;
			pipe->img_width /= 2;
		} else {
			pipe->src.h /= 2;
		}
	}

	ret = __mdp_pipe_tune_perf(pipe);
	if (ret) {
		pr_debug("unable to satisfy performance. ret=%d\n", ret);
		goto exit_fail;
	}

	ret = __mdss_mdp_overlay_setup_scaling(pipe);
	if (ret)
		goto exit_fail;

	if ((mixer->type == MDSS_MDP_MIXER_TYPE_WRITEBACK) &&
		!mdp5_data->mdata->has_wfd_blk)
		mdss_mdp_smp_release(pipe);

	ret = mdss_mdp_smp_reserve(pipe);
	if (ret) {
		pr_debug("mdss_mdp_smp_reserve failed. ret=%d\n", ret);
		goto exit_fail;
	}

	pipe->params_changed++;

	req->vert_deci = pipe->vert_deci;

	*ppipe = pipe;

	mdss_mdp_pipe_unmap(pipe);

	return ret;

exit_fail:
	mdss_mdp_pipe_unmap(pipe);

	mutex_lock(&mfd->lock);
	if (pipe->play_cnt == 0) {
		pr_debug("failed for pipe %d\n", pipe->num);
		if (!list_empty(&pipe->used_list))
			list_del_init(&pipe->used_list);
		mdss_mdp_pipe_destroy(pipe);
	}

	/* invalidate any overlays in this framebuffer after failure */
	list_for_each_entry(pipe, &mdp5_data->pipes_used, used_list) {
		pr_debug("freeing allocations for pipe %d\n", pipe->num);
		mdss_mdp_smp_unreserve(pipe);
		pipe->params_changed = 0;
	}
	mutex_unlock(&mfd->lock);
	return ret;
}

static int mdss_mdp_overlay_set(struct msm_fb_data_type *mfd,
				struct mdp_overlay *req)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret;

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (!mfd->panel_power_on) {
		mutex_unlock(&mdp5_data->ov_lock);
		return -EPERM;
	}

	if (req->flags & MDSS_MDP_ROT_ONLY) {
		ret = mdss_mdp_overlay_rotator_setup(mfd, req);
	} else if (req->src.format == MDP_RGB_BORDERFILL) {
		req->id = BORDERFILL_NDX;
	} else {
		struct mdss_mdp_pipe *pipe;

		/* userspace zorder start with stage 0 */
		req->z_order += MDSS_MDP_STAGE_0;

		ret = mdss_mdp_overlay_pipe_setup(mfd, req, &pipe);

		req->z_order -= MDSS_MDP_STAGE_0;
	}

	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

static inline int mdss_mdp_overlay_get_buf(struct msm_fb_data_type *mfd,
					   struct mdss_mdp_data *data,
					   struct msmfb_data *planes,
					   int num_planes,
					   u32 flags)
{
	int i, rc = 0;

	if ((num_planes <= 0) || (num_planes > MAX_PLANES))
		return -EINVAL;

	memset(data, 0, sizeof(*data));
	for (i = 0; i < num_planes; i++) {
		data->p[i].flags = flags;
		rc = mdss_mdp_get_img(&planes[i], &data->p[i]);
		if (rc) {
			pr_err("failed to map buf p=%d flags=%x\n", i, flags);
			while (i > 0) {
				i--;
				mdss_mdp_put_img(&data->p[i]);
			}
			break;
		}
	}

	data->num_planes = i;

	return rc;
}

static inline int mdss_mdp_overlay_free_buf(struct mdss_mdp_data *data)
{
	int i;
	for (i = 0; i < data->num_planes && data->p[i].len; i++)
		mdss_mdp_put_img(&data->p[i]);

	data->num_planes = 0;

	return 0;
}

static void mdss_mdp_overlay_cleanup(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_pipe *pipe, *tmp;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	LIST_HEAD(destroy_pipes);

	mutex_lock(&mfd->lock);
	list_for_each_entry_safe(pipe, tmp, &mdp5_data->pipes_cleanup,
				cleanup_list) {
		list_move(&pipe->cleanup_list, &destroy_pipes);
		mdss_mdp_overlay_free_buf(&pipe->back_buf);
		mdss_mdp_overlay_free_buf(&pipe->front_buf);
		pipe->mfd = NULL;
	}

	list_for_each_entry(pipe, &mdp5_data->pipes_used, used_list) {
		if (pipe->back_buf.num_planes) {
			/* make back buffer active */
			mdss_mdp_overlay_free_buf(&pipe->front_buf);
			swap(pipe->back_buf, pipe->front_buf);
		}
	}
	mutex_unlock(&mfd->lock);
	list_for_each_entry_safe(pipe, tmp, &destroy_pipes, cleanup_list)
		mdss_mdp_pipe_destroy(pipe);
}

static int mdss_mdp_overlay_start(struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	if (mdp5_data->ctl->power_on)
		return 0;

	pr_debug("starting fb%d overlay\n", mfd->index);

	rc = pm_runtime_get_sync(&mfd->pdev->dev);
	if (IS_ERR_VALUE(rc)) {
		pr_err("unable to resume with pm_runtime_get_sync rc=%d\n", rc);
		return rc;
	}

	if (mfd->panel_info->cont_splash_enabled) {
		mdss_mdp_ctl_splash_finish(mdp5_data->ctl);
		mdss_mdp_footswitch_ctrl_splash(0);
	}

	if (!is_mdss_iommu_attached()) {
		mdss_iommu_attach(mdss_res);
		mdss_hw_init(mdss_res);
	}

	rc = mdss_mdp_ctl_start(mdp5_data->ctl);
	if (rc == 0) {
		atomic_inc(&ov_active_panels);

		mdss_mdp_ctl_notifier_register(mdp5_data->ctl,
				&mfd->mdp_sync_pt_data.notifier);
	} else {
		pr_err("overlay start failed.\n");
		mdss_mdp_ctl_destroy(mdp5_data->ctl);
		mdp5_data->ctl = NULL;

		pm_runtime_put(&mfd->pdev->dev);
	}

	return rc;
}

static void mdss_mdp_overlay_update_pm(struct mdss_overlay_private *mdp5_data)
{
	ktime_t wakeup_time;

	if (!mdp5_data->cpu_pm_hdl)
		return;

	if (mdss_mdp_display_wakeup_time(mdp5_data->ctl, &wakeup_time))
		return;

	activate_event_timer(mdp5_data->cpu_pm_hdl, wakeup_time);
}

int mdss_mdp_overlay_kickoff(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_mdp_ctl *tmp;
	int ret = 0;
	if (ctl->shared_lock)
		mutex_lock(ctl->shared_lock);

	mutex_lock(&mdp5_data->ov_lock);
	mutex_lock(&mfd->lock);

	mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_BEGIN);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);

	list_for_each_entry(pipe, &mdp5_data->pipes_used, used_list) {
		struct mdss_mdp_data *buf;
		/*
		 * When external is connected and no dedicated wfd is present,
		 * reprogram DMA pipe before kickoff to clear out any previous
		 * block mode configuration.
		 */
		if ((pipe->type == MDSS_MDP_PIPE_TYPE_DMA) &&
		    (ctl->shared_lock && !ctl->mdata->has_wfd_blk)) {
			if (ctl->mdata->mixer_switched) {
				ret = mdss_mdp_overlay_pipe_setup(mfd,
						&pipe->req_data, &pipe);
				pr_debug("reseting DMA pipe for ctl=%d",
					 ctl->num);
			}
			if (ret) {
				pr_err("can't reset DMA pipe ret=%d ctl=%d\n",
					ret, ctl->num);
				mutex_unlock(&mfd->lock);
				goto commit_fail;
			}

			tmp = mdss_mdp_ctl_mixer_switch(ctl,
					MDSS_MDP_WB_CTL_TYPE_LINE);
			if (!tmp) {
				mutex_unlock(&mfd->lock);
				ret = -EINVAL;
				goto commit_fail;
			}
			pipe->mixer = mdss_mdp_mixer_get(tmp,
					MDSS_MDP_MIXER_MUX_DEFAULT);
		}
		if (pipe->back_buf.num_planes) {
			buf = &pipe->back_buf;
		} else if (ctl->play_cnt == 0 && pipe->front_buf.num_planes) {
			pipe->params_changed++;
			buf = &pipe->front_buf;
		} else if (!pipe->params_changed) {
			continue;
		} else if (pipe->front_buf.num_planes) {
			buf = &pipe->front_buf;
		} else {
			pr_warn("pipe queue w/o buffer\n");
			continue;
		}

		ret = mdss_mdp_pipe_queue_data(pipe, buf);
		if (IS_ERR_VALUE(ret)) {
			pr_warn("Unable to queue data for pnum=%d\n",
					pipe->num);
			mdss_mdp_mixer_pipe_unstage(pipe);
		}
	}

	if (mfd->panel.type == WRITEBACK_PANEL)
		ret = mdss_mdp_wb_kickoff(mfd);
	else
		ret = mdss_mdp_display_commit(mdp5_data->ctl, NULL);

	mutex_unlock(&mfd->lock);

	if (IS_ERR_VALUE(ret))
		goto commit_fail;

	mdss_mdp_overlay_update_pm(mdp5_data);

	ret = mdss_mdp_display_wait4comp(mdp5_data->ctl);

	mdss_fb_update_notify_update(mfd);
commit_fail:
	mdss_mdp_overlay_cleanup(mfd);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
	if (ctl)
		mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_FLUSHED);

	mutex_unlock(&mdp5_data->ov_lock);
	if (ctl && ctl->shared_lock)
		mutex_unlock(ctl->shared_lock);

	return ret;
}

static int mdss_mdp_overlay_release(struct msm_fb_data_type *mfd, int ndx)
{
	struct mdss_mdp_pipe *pipe;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u32 pipe_ndx, unset_ndx = 0;
	int i;

	for (i = 0; unset_ndx != ndx && i < MDSS_MDP_MAX_SSPP; i++) {
		pipe_ndx = BIT(i);
		if (pipe_ndx & ndx) {
			unset_ndx |= pipe_ndx;
			pipe = mdss_mdp_pipe_get(mdp5_data->mdata, pipe_ndx);
			if (IS_ERR_OR_NULL(pipe)) {
				pr_warn("unknown pipe ndx=%x\n", pipe_ndx);
				continue;
			}
			mutex_lock(&mfd->lock);
			pipe->pid = 0;
			if (!list_empty(&pipe->used_list)) {
				list_del_init(&pipe->used_list);
				list_add(&pipe->cleanup_list,
					&mdp5_data->pipes_cleanup);
			}
			mutex_unlock(&mfd->lock);
			mdss_mdp_mixer_pipe_unstage(pipe);
			mdss_mdp_pipe_unmap(pipe);
		}
	}
	return 0;
}

static int mdss_mdp_overlay_unset(struct msm_fb_data_type *mfd, int ndx)
{
	int ret = 0;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd)
		return -ENODEV;

	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl)
		return -ENODEV;

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (ndx == BORDERFILL_NDX) {
		pr_debug("borderfill disable\n");
		mdp5_data->borderfill_enable = false;
		ret = 0;
		goto done;
	}

	if (!mfd->panel_power_on) {
		ret = -EPERM;
		goto done;
	}

	pr_debug("unset ndx=%x\n", ndx);

	if (ndx & MDSS_MDP_ROT_SESSION_MASK) {
		struct mdss_mdp_rotator_session *rot;
		rot = mdss_mdp_rotator_session_get(ndx);
		if (rot) {
			mdss_mdp_overlay_free_buf(&rot->src_buf);
			mdss_mdp_overlay_free_buf(&rot->dst_buf);

			rot->pid = 0;
			if (!list_empty(&rot->list))
				list_del_init(&rot->list);
			ret = mdss_mdp_rotator_release(rot);
		}
	} else {
		ret = mdss_mdp_overlay_release(mfd, ndx);
	}

done:
	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

/**
 * mdss_mdp_overlay_release_all() - release any overlays associated with fb dev
 * @mfd:	Msm frame buffer structure associated with fb device
 * @release_all: ignore pid and release all the pipes
 *
 * Release any resources allocated by calling process, this can be called
 * on fb_release to release any overlays/rotator sessions left open.
 */
static int __mdss_mdp_overlay_release_all(struct msm_fb_data_type *mfd,
	bool release_all)
{
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_rotator_session *rot, *tmp;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u32 unset_ndx = 0;
	int cnt = 0;
	int pid = current->tgid;

	pr_debug("releasing all resources for fb%d pid=%d\n", mfd->index, pid);

	mutex_lock(&mdp5_data->ov_lock);
	mutex_lock(&mfd->lock);
	list_for_each_entry(pipe, &mdp5_data->pipes_used, used_list) {
		if (release_all || (pipe->pid == pid)) {
			unset_ndx |= pipe->ndx;
			cnt++;
		}
	}

	if (cnt == 0 && !list_empty(&mdp5_data->pipes_cleanup)) {
		pr_debug("overlay release on fb%d called without commit!",
			mfd->index);
		cnt++;
	}

	pr_debug("release_all=%d mfd->ref_cnt=%d unset_ndx=0x%x cnt=%d\n",
		release_all, mfd->ref_cnt, unset_ndx, cnt);

	mutex_unlock(&mfd->lock);

	if (unset_ndx) {
		pr_debug("%d pipes need cleanup (%x)\n", cnt, unset_ndx);
		mdss_mdp_overlay_release(mfd, unset_ndx);
	}
	mutex_unlock(&mdp5_data->ov_lock);

	if (cnt)
		mfd->mdp.kickoff_fnc(mfd);

	list_for_each_entry_safe(rot, tmp, &mdp5_data->rot_proc_list, list) {
		if (rot->pid == pid) {
			if (!list_empty(&rot->list))
				list_del_init(&rot->list);
			mdss_mdp_rotator_release(rot);
		}
	}

	return 0;
}

static int mdss_mdp_overlay_play_wait(struct msm_fb_data_type *mfd,
				      struct msmfb_overlay_data *req)
{
	int ret = 0;

	if (!mfd)
		return -ENODEV;

	ret = mfd->mdp.kickoff_fnc(mfd);
	if (!ret)
		pr_err("error displaying\n");

	return ret;
}

static int mdss_mdp_overlay_rotate(struct msm_fb_data_type *mfd,
				   struct msmfb_overlay_data *req)
{
	struct mdss_mdp_rotator_session *rot;
	int ret;
	u32 flgs;

	rot = mdss_mdp_rotator_session_get(req->id);
	if (!rot) {
		pr_err("invalid session id=%x\n", req->id);
		return -ENOENT;
	}

	flgs = rot->flags & MDP_SECURE_OVERLAY_SESSION;

	ret = mdss_mdp_rotator_busy_wait_ex(rot);
	if (ret) {
		pr_err("rotator busy wait error\n");
		return ret;
	}

	mdss_mdp_overlay_free_buf(&rot->src_buf);
	ret = mdss_mdp_overlay_get_buf(mfd, &rot->src_buf, &req->data, 1, flgs);
	if (ret) {
		pr_err("src_data pmem error\n");
		return ret;
	}

	mdss_mdp_overlay_free_buf(&rot->dst_buf);
	ret = mdss_mdp_overlay_get_buf(mfd, &rot->dst_buf,
			&req->dst_data, 1, flgs);
	if (ret) {
		pr_err("dst_data pmem error\n");
		goto dst_buf_fail;
	}

	ret = mdss_mdp_rotator_queue(rot);
	if (ret)
		pr_err("rotator queue error session id=%x\n", req->id);

dst_buf_fail:
	return ret;
}

static int mdss_mdp_overlay_queue(struct msm_fb_data_type *mfd,
				  struct msmfb_overlay_data *req)
{
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_data *src_data;
	int ret;
	u32 flags;
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);

	pipe = mdss_mdp_pipe_get(mdata, req->id);
	if (IS_ERR_OR_NULL(pipe)) {
		pr_err("pipe ndx=%x doesn't exist\n", req->id);
		return pipe ? PTR_ERR(pipe) : -ENODEV;
	}

	pr_debug("ov queue pnum=%d\n", pipe->num);

	flags = (pipe->flags & MDP_SECURE_OVERLAY_SESSION);

	src_data = &pipe->back_buf;
	if (src_data->num_planes) {
		pr_warn("dropped buffer pnum=%d play=%d addr=0x%x\n",
			pipe->num, pipe->play_cnt, src_data->p[0].addr);
		mdss_mdp_overlay_free_buf(src_data);
	}

	ret = mdss_mdp_overlay_get_buf(mfd, src_data, &req->data, 1, flags);
	if (IS_ERR_VALUE(ret)) {
		pr_err("src_data pmem error\n");
	}
	mdss_mdp_pipe_unmap(pipe);

	return ret;
}

static void mdss_mdp_overlay_force_cleanup(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	int ret;

	pr_debug("forcing cleanup to unset dma pipes on fb%d\n", mfd->index);

	/*
	 * video mode panels require the layer to be unstaged and wait for
	 * vsync to be able to release buffer.
	 */
	if (ctl && ctl->is_video_mode) {
		ret = mdss_mdp_display_commit(ctl, NULL);
		if (!IS_ERR_VALUE(ret))
			mdss_mdp_display_wait4comp(ctl);
	}

	mdss_mdp_overlay_cleanup(mfd);
}

static void mdss_mdp_overlay_force_dma_cleanup(struct mdss_data_type *mdata)
{
	struct mdss_mdp_pipe *pipe;
	int i;

	for (i = 0; i < mdata->ndma_pipes; i++) {
		pipe = mdata->dma_pipes + i;
		if (atomic_read(&pipe->ref_cnt) && pipe->mfd)
			mdss_mdp_overlay_force_cleanup(pipe->mfd);
	}
}

static int mdss_mdp_overlay_play(struct msm_fb_data_type *mfd,
				 struct msmfb_overlay_data *req)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret = 0;

	pr_debug("play req id=%x\n", req->id);

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (!mfd->panel_power_on) {
		ret = -EPERM;
		goto done;
	}

	ret = mdss_mdp_overlay_start(mfd);
	if (ret) {
		pr_err("unable to start overlay %d (%d)\n", mfd->index, ret);
		goto done;
	}

	if (req->id & MDSS_MDP_ROT_SESSION_MASK) {
		mdss_mdp_overlay_force_dma_cleanup(mfd_to_mdata(mfd));

		ret = mdss_mdp_overlay_rotate(mfd, req);
	} else if (req->id == BORDERFILL_NDX) {
		pr_debug("borderfill enable\n");
		mdp5_data->borderfill_enable = true;
		ret = mdss_mdp_overlay_free_fb_pipe(mfd);
	} else {
		ret = mdss_mdp_overlay_queue(mfd, req);
	}

done:
	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

static int mdss_mdp_overlay_free_fb_pipe(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_pipe *pipe;
	u32 fb_ndx = 0;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	pipe = mdss_mdp_mixer_stage_pipe(mdp5_data->ctl,
					MDSS_MDP_MIXER_MUX_LEFT,
					 MDSS_MDP_STAGE_BASE);
	if (pipe)
		fb_ndx |= pipe->ndx;

	pipe = mdss_mdp_mixer_stage_pipe(mdp5_data->ctl,
					MDSS_MDP_MIXER_MUX_RIGHT,
					 MDSS_MDP_STAGE_BASE);
	if (pipe)
		fb_ndx |= pipe->ndx;

	if (fb_ndx) {
		pr_debug("unstaging framebuffer pipes %x\n", fb_ndx);
		mdss_mdp_overlay_release(mfd, fb_ndx);
	}
	return 0;
}

static int mdss_mdp_overlay_get_fb_pipe(struct msm_fb_data_type *mfd,
					struct mdss_mdp_pipe **ppipe,
					int mixer_mux)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;

	pipe = mdss_mdp_mixer_stage_pipe(mdp5_data->ctl, mixer_mux,
					 MDSS_MDP_STAGE_BASE);
	if (pipe == NULL) {
		struct mdp_overlay req;
		struct fb_info *fbi = mfd->fbi;
		struct mdss_mdp_mixer *mixer;
		int ret, bpp;

		mixer = mdss_mdp_mixer_get(mdp5_data->ctl,
					MDSS_MDP_MIXER_MUX_LEFT);
		if (!mixer) {
			pr_err("unable to retrieve mixer\n");
			return -ENODEV;
		}

		memset(&req, 0, sizeof(req));

		bpp = fbi->var.bits_per_pixel / 8;
		req.id = MSMFB_NEW_REQUEST;
		req.src.format = mfd->fb_imgType;
		req.src.height = fbi->var.yres;
		req.src.width = fbi->fix.line_length / bpp;
		if (mixer_mux == MDSS_MDP_MIXER_MUX_RIGHT) {
			if (req.src.width <= mixer->width) {
				pr_warn("right fb pipe not needed\n");
				return -EINVAL;
			}

			req.flags |= MDSS_MDP_RIGHT_MIXER;
			req.src_rect.x = mixer->width;
			req.src_rect.w = fbi->var.xres - mixer->width;
		} else {
			req.src_rect.x = 0;
			req.src_rect.w = MIN(fbi->var.xres, mixer->width);
		}

		req.src_rect.y = 0;
		req.src_rect.h = req.src.height;
		req.dst_rect.x = 0;
		req.dst_rect.y = 0;
		req.dst_rect.w = req.src_rect.w;
		req.dst_rect.h = req.src_rect.h;
		req.z_order = MDSS_MDP_STAGE_BASE;

		pr_debug("allocating base pipe mux=%d\n", mixer_mux);

		ret = mdss_mdp_overlay_pipe_setup(mfd, &req, &pipe);
		if (ret)
			return ret;

		pr_debug("ctl=%d pnum=%d\n", mdp5_data->ctl->num, pipe->num);
	}

	*ppipe = pipe;
	return 0;
}

static void mdss_mdp_overlay_pan_display(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_data *buf;
	struct mdss_mdp_pipe *pipe;
	struct fb_info *fbi;
	struct mdss_overlay_private *mdp5_data;
	u32 offset;
	int bpp, ret;

	if (!mfd)
		return;

	fbi = mfd->fbi;
	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl)
		return;

	if (!fbi->fix.smem_start || fbi->fix.smem_len == 0 ||
	     mdp5_data->borderfill_enable) {
		mfd->mdp.kickoff_fnc(mfd);
		return;
	}

	if (mutex_lock_interruptible(&mdp5_data->ov_lock))
		return;

	if (!mfd->panel_power_on) {
		mutex_unlock(&mdp5_data->ov_lock);
		return;
	}

	bpp = fbi->var.bits_per_pixel / 8;
	offset = fbi->var.xoffset * bpp +
		 fbi->var.yoffset * fbi->fix.line_length;

	if (offset > fbi->fix.smem_len) {
		pr_err("invalid fb offset=%u total length=%u\n",
		       offset, fbi->fix.smem_len);
		goto pan_display_error;
	}

	ret = mdss_mdp_overlay_start(mfd);
	if (ret) {
		pr_err("unable to start overlay %d (%d)\n", mfd->index, ret);
		goto pan_display_error;
	}


	ret = mdss_mdp_overlay_get_fb_pipe(mfd, &pipe, MDSS_MDP_MIXER_MUX_LEFT);
	if (ret) {
		pr_err("unable to allocate base pipe\n");
		goto pan_display_error;
	}

	if (mdss_mdp_pipe_map(pipe)) {
		pr_err("unable to map base pipe\n");
		goto pan_display_error;
	}

	buf = &pipe->back_buf;
	if (is_mdss_iommu_attached()) {
		if (!mfd->iova) {
			pr_err("mfd iova is zero\n");
			goto pan_display_error;
		}
		buf->p[0].addr = mfd->iova;
	} else {
		buf->p[0].addr = fbi->fix.smem_start;
	}

	buf->p[0].addr += offset;
	buf->p[0].len = fbi->fix.smem_len - offset;
	buf->num_planes = 1;
	mdss_mdp_pipe_unmap(pipe);

	if (fbi->var.xres > MAX_MIXER_WIDTH || mfd->split_display) {
		ret = mdss_mdp_overlay_get_fb_pipe(mfd, &pipe,
						   MDSS_MDP_MIXER_MUX_RIGHT);
		if (ret) {
			pr_err("unable to allocate right base pipe\n");
			goto pan_display_error;
		}
		if (mdss_mdp_pipe_map(pipe)) {
			pr_err("unable to map right base pipe\n");
			goto pan_display_error;
		}
		pipe->back_buf = *buf;
		mdss_mdp_pipe_unmap(pipe);
	}
	mutex_unlock(&mdp5_data->ov_lock);

	if ((fbi->var.activate & FB_ACTIVATE_VBL) ||
	    (fbi->var.activate & FB_ACTIVATE_FORCE))
		mfd->mdp.kickoff_fnc(mfd);

	return;

pan_display_error:
	mutex_unlock(&mdp5_data->ov_lock);
}

/* function is called in irq context should have minimum processing */
static void mdss_mdp_overlay_handle_vsync(struct mdss_mdp_ctl *ctl,
						ktime_t t)
{
	struct msm_fb_data_type *mfd = ctl->mfd;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd || !mfd->mdp.private1) {
		pr_warn("Invalid handle for vsync\n");
		return;
	}

	mdp5_data = mfd_to_mdp5_data(mfd);
	pr_debug("vsync on fb%d play_cnt=%d\n", mfd->index, ctl->play_cnt);

	mdp5_data->vsync_time = t;
	sysfs_notify_dirent(mdp5_data->vsync_event_sd);
}

int mdss_mdp_overlay_vsync_ctrl(struct msm_fb_data_type *mfd, int en)
{
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	int rc;

	if (!ctl)
		return -ENODEV;
	if (!ctl->add_vsync_handler || !ctl->remove_vsync_handler)
		return -EOPNOTSUPP;

	if (!ctl->power_on) {
		pr_debug("fb%d vsync pending first update en=%d\n",
				mfd->index, en);
		return -EPERM;
	}

	pr_debug("fb%d vsync en=%d\n", mfd->index, en);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	if (en)
		rc = ctl->add_vsync_handler(ctl, &ctl->vsync_handler);
	else
		rc = ctl->remove_vsync_handler(ctl, &ctl->vsync_handler);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	return rc;
}

static ssize_t mdss_mdp_vsync_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u64 vsync_ticks;
	int ret;

	if (!mdp5_data->ctl || !mdp5_data->ctl->power_on)
		return -EAGAIN;

	vsync_ticks = ktime_to_ns(mdp5_data->vsync_time);

	pr_debug("fb%d vsync=%llu", mfd->index, vsync_ticks);
	ret = scnprintf(buf, PAGE_SIZE, "VSYNC=%llu", vsync_ticks);

	return ret;
}

static DEVICE_ATTR(vsync_event, S_IRUGO, mdss_mdp_vsync_show_event, NULL);

static struct attribute *vsync_fs_attrs[] = {
	&dev_attr_vsync_event.attr,
	NULL,
};

static struct attribute_group vsync_fs_attr_group = {
	.attrs = vsync_fs_attrs,
};

static int mdss_mdp_hw_cursor_update(struct msm_fb_data_type *mfd,
				     struct fb_cursor *cursor)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_mixer *mixer;
	struct fb_image *img = &cursor->image;
	u32 blendcfg;
	int off, ret = 0;

	if (!mfd->cursor_buf && (cursor->set & FB_CUR_SETIMAGE)) {
		mfd->cursor_buf = dma_alloc_coherent(NULL, MDSS_MDP_CURSOR_SIZE,
					(dma_addr_t *) &mfd->cursor_buf_phys,
					GFP_KERNEL);
		if (!mfd->cursor_buf) {
			pr_err("can't allocate cursor buffer\n");
			return -ENOMEM;
		}

		ret = msm_iommu_map_contig_buffer(mfd->cursor_buf_phys,
			mdss_get_iommu_domain(MDSS_IOMMU_DOMAIN_UNSECURE),
			0, MDSS_MDP_CURSOR_SIZE, SZ_4K, 0,
			&(mfd->cursor_buf_iova));
		if (IS_ERR_VALUE(ret)) {
			dma_free_coherent(NULL, MDSS_MDP_CURSOR_SIZE,
					  mfd->cursor_buf,
					  (dma_addr_t) mfd->cursor_buf_phys);
			pr_err("unable to map cursor buffer to iommu(%d)\n",
			       ret);
			return -ENOMEM;
		}
	}

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_DEFAULT);
	off = MDSS_MDP_REG_LM_OFFSET(mixer->num);

	if ((img->width > MDSS_MDP_CURSOR_WIDTH) ||
		(img->height > MDSS_MDP_CURSOR_HEIGHT) ||
		(img->depth != 32))
		return -EINVAL;

	pr_debug("mixer=%d enable=%x set=%x\n", mixer->num, cursor->enable,
			cursor->set);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	blendcfg = MDSS_MDP_REG_READ(off + MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG);

	if (cursor->set & FB_CUR_SETPOS)
		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_START_XY,
				   (img->dy << 16) | img->dx);

	if (cursor->set & FB_CUR_SETIMAGE) {
		int calpha_en, transp_en, alpha, size, cursor_addr;
		ret = copy_from_user(mfd->cursor_buf, img->data,
				     img->width * img->height * 4);
		if (ret)
			return ret;

		if (is_mdss_iommu_attached())
			cursor_addr = mfd->cursor_buf_iova;
		else
			cursor_addr = mfd->cursor_buf_phys;

		if (img->bg_color == 0xffffffff)
			transp_en = 0;
		else
			transp_en = 1;

		alpha = (img->fg_color & 0xff000000) >> 24;

		if (alpha)
			calpha_en = 0x0; /* xrgb */
		else
			calpha_en = 0x2; /* argb */

		size = (img->height << 16) | img->width;
		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_IMG_SIZE, size);
		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_SIZE, size);
		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_STRIDE,
				   img->width * 4);
		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_BASE_ADDR,
				   cursor_addr);

		wmb();

		blendcfg &= ~0x1;
		blendcfg |= (transp_en << 3) | (calpha_en << 1);
		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG,
				   blendcfg);
		if (calpha_en)
			MDSS_MDP_REG_WRITE(off +
					   MDSS_MDP_REG_LM_CURSOR_BLEND_PARAM,
					   alpha);

		if (transp_en) {
			MDSS_MDP_REG_WRITE(off +
				   MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_LOW0,
				   ((img->bg_color & 0xff00) << 8) |
				   (img->bg_color & 0xff));
			MDSS_MDP_REG_WRITE(off +
				   MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_LOW1,
				   ((img->bg_color & 0xff0000) >> 16));
			MDSS_MDP_REG_WRITE(off +
				   MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_HIGH0,
				   ((img->bg_color & 0xff00) << 8) |
				   (img->bg_color & 0xff));
			MDSS_MDP_REG_WRITE(off +
				   MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_HIGH1,
				   ((img->bg_color & 0xff0000) >> 16));
		}
	}

	if (!cursor->enable != !(blendcfg & 0x1)) {
		if (cursor->enable) {
			pr_debug("enable hw cursor on mixer=%d\n", mixer->num);
			blendcfg |= 0x1;
		} else {
			pr_debug("disable hw cursor on mixer=%d\n", mixer->num);
			blendcfg &= ~0x1;
		}

		MDSS_MDP_REG_WRITE(off + MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG,
				   blendcfg);

		mixer->cursor_enabled = cursor->enable;
		mixer->params_changed++;
	}

	mixer->ctl->flush_bits |= BIT(6) << mixer->num;
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	return 0;
}

static int mdss_bl_scale_config(struct msm_fb_data_type *mfd,
						struct mdp_bl_scale_data *data)
{
	int ret = 0;
	int curr_bl;
	mutex_lock(&mfd->bl_lock);
	curr_bl = mfd->bl_level;
	mfd->bl_scale = data->scale;
	mfd->bl_min_lvl = data->min_lvl;
	pr_debug("update scale = %d, min_lvl = %d\n", mfd->bl_scale,
							mfd->bl_min_lvl);

	/* update current backlight to use new scaling*/
	mdss_fb_set_backlight(mfd, curr_bl);
	mutex_unlock(&mfd->bl_lock);
	return ret;
}

static int mdss_mdp_pp_ioctl(struct msm_fb_data_type *mfd,
				void __user *argp)
{
	int ret;
	struct msmfb_mdp_pp mdp_pp;
	u32 copyback = 0;
	u32 copy_from_kernel = 0;

	ret = copy_from_user(&mdp_pp, argp, sizeof(mdp_pp));
	if (ret)
		return ret;

	/* Supprt only MDP register read/write and
	exit_dcm in DCM state*/
	if (mfd->dcm_state == DCM_ENTER &&
			(mdp_pp.op != mdp_op_calib_buffer &&
			mdp_pp.op != mdp_op_calib_dcm_state))
		return -EPERM;

	switch (mdp_pp.op) {
	case mdp_op_pa_cfg:
		ret = mdss_mdp_pa_config(&mdp_pp.data.pa_cfg_data,
					&copyback);
		break;

	case mdp_op_pcc_cfg:
		ret = mdss_mdp_pcc_config(&mdp_pp.data.pcc_cfg_data,
					&copyback);
		break;

	case mdp_op_lut_cfg:
		switch (mdp_pp.data.lut_cfg_data.lut_type) {
		case mdp_lut_igc:
			ret = mdss_mdp_igc_lut_config(
					(struct mdp_igc_lut_data *)
					&mdp_pp.data.lut_cfg_data.data,
					&copyback, copy_from_kernel);
			break;

		case mdp_lut_pgc:
			ret = mdss_mdp_argc_config(
				&mdp_pp.data.lut_cfg_data.data.pgc_lut_data,
				&copyback);
			break;

		case mdp_lut_hist:
			ret = mdss_mdp_hist_lut_config(
				(struct mdp_hist_lut_data *)
				&mdp_pp.data.lut_cfg_data.data, &copyback);
			break;

		default:
			ret = -ENOTSUPP;
			break;
		}
		break;
	case mdp_op_dither_cfg:
		ret = mdss_mdp_dither_config(
				&mdp_pp.data.dither_cfg_data,
				&copyback);
		break;
	case mdp_op_gamut_cfg:
		ret = mdss_mdp_gamut_config(
				&mdp_pp.data.gamut_cfg_data,
				&copyback);
		break;
	case mdp_bl_scale_cfg:
		ret = mdss_bl_scale_config(mfd, (struct mdp_bl_scale_data *)
						&mdp_pp.data.bl_scale_data);
		break;
	case mdp_op_ad_cfg:
		ret = mdss_mdp_ad_config(mfd, &mdp_pp.data.ad_init_cfg);
		break;
	case mdp_op_ad_input:
		ret = mdss_mdp_ad_input(mfd, &mdp_pp.data.ad_input, 1);
		if (ret > 0) {
			ret = 0;
			copyback = 1;
		}
		break;
	case mdp_op_calib_cfg:
		ret = mdss_mdp_calib_config((struct mdp_calib_config_data *)
					 &mdp_pp.data.calib_cfg, &copyback);
		break;
	case mdp_op_calib_mode:
		ret = mdss_mdp_calib_mode(mfd, &mdp_pp.data.mdss_calib_cfg);
		break;
	case mdp_op_calib_buffer:
		ret = mdss_mdp_calib_config_buffer(
				(struct mdp_calib_config_buffer *)
				 &mdp_pp.data.calib_buffer, &copyback);
		break;
	case mdp_op_calib_dcm_state:
		ret = mdss_fb_dcm(mfd, mdp_pp.data.calib_dcm.dcm_state);
		break;
	default:
		pr_err("Unsupported request to MDP_PP IOCTL. %d = op\n",
								mdp_pp.op);
		ret = -EINVAL;
		break;
	}
	if ((ret == 0) && copyback)
		ret = copy_to_user(argp, &mdp_pp, sizeof(struct msmfb_mdp_pp));
	return ret;
}

static int mdss_mdp_histo_ioctl(struct msm_fb_data_type *mfd, u32 cmd,
				void __user *argp)
{
	int ret = -ENOSYS;
	struct mdp_histogram_data hist;
	struct mdp_histogram_start_req hist_req;
	u32 block;

	switch (cmd) {
	case MSMFB_HISTOGRAM_START:
		if (!mfd->panel_power_on)
			return -EPERM;

		ret = copy_from_user(&hist_req, argp, sizeof(hist_req));
		if (ret)
			return ret;

		ret = mdss_mdp_histogram_start(&hist_req);
		break;

	case MSMFB_HISTOGRAM_STOP:
		ret = copy_from_user(&block, argp, sizeof(int));
		if (ret)
			return ret;

		ret = mdss_mdp_histogram_stop(block);
		break;

	case MSMFB_HISTOGRAM:
		if (!mfd->panel_power_on)
			return -EPERM;

		ret = copy_from_user(&hist, argp, sizeof(hist));
		if (ret)
			return ret;

		ret = mdss_mdp_hist_collect(&hist);
		if (!ret)
			ret = copy_to_user(argp, &hist, sizeof(hist));
		break;
	default:
		break;
	}
	return ret;
}

static int mdss_fb_set_metadata(struct msm_fb_data_type *mfd,
				struct msmfb_metadata *metadata)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	int ret = 0;
	switch (metadata->op) {
	case metadata_op_vic:
		if (mfd->panel_info)
			mfd->panel_info->vic =
				metadata->data.video_info_code;
		else
			ret = -EINVAL;
		break;
	case metadata_op_crc:
		if (!mfd->panel_power_on)
			return -EPERM;
		ret = mdss_misr_crc_set(mdata, &metadata->data.misr_request);
		break;
	case metadata_op_wb_format:
		ret = mdss_mdp_wb_set_format(mfd,
				metadata->data.mixer_cfg.writeback_format);
		break;
	default:
		pr_warn("unsupported request to MDP META IOCTL\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int mdss_fb_get_hw_caps(struct msm_fb_data_type *mfd,
		struct mdss_hw_caps *caps)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	caps->mdp_rev = mdata->mdp_rev;
	caps->vig_pipes = mdata->nvig_pipes;
	caps->rgb_pipes = mdata->nrgb_pipes;
	caps->dma_pipes = mdata->ndma_pipes;
	if (mdata->has_bwc)
		caps->features |= MDP_BWC_EN;
	if (mdata->has_decimation)
		caps->features |= MDP_DECIMATION_EN;
	return 0;
}

static int mdss_fb_get_metadata(struct msm_fb_data_type *mfd,
				struct msmfb_metadata *metadata)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	int ret = 0;
	switch (metadata->op) {
	case metadata_op_frame_rate:
		metadata->data.panel_frame_rate =
			mdss_panel_get_framerate(mfd->panel_info);
		break;
	case metadata_op_get_caps:
		ret = mdss_fb_get_hw_caps(mfd, &metadata->data.caps);
		break;
	case metadata_op_crc:
		if (!mfd->panel_power_on)
			return -EPERM;
		ret = mdss_misr_crc_get(mdata, &metadata->data.misr_request);
		break;
	case metadata_op_wb_format:
		ret = mdss_mdp_wb_get_format(mfd, &metadata->data.mixer_cfg);
		break;
	default:
		pr_warn("Unsupported request to MDP META IOCTL.\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int mdss_mdp_overlay_ioctl_handler(struct msm_fb_data_type *mfd,
					  u32 cmd, void __user *argp)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdp_overlay req;
	int val, ret = -ENOSYS;
	struct msmfb_metadata metadata;

	switch (cmd) {
	case MSMFB_MDP_PP:
		ret = mdss_mdp_pp_ioctl(mfd, argp);
		break;

	case MSMFB_HISTOGRAM_START:
	case MSMFB_HISTOGRAM_STOP:
	case MSMFB_HISTOGRAM:
		ret = mdss_mdp_histo_ioctl(mfd, cmd, argp);
		break;

	case MSMFB_OVERLAY_GET:
		ret = copy_from_user(&req, argp, sizeof(req));
		if (!ret) {
			ret = mdss_mdp_overlay_get(mfd, &req);

			if (!IS_ERR_VALUE(ret))
				ret = copy_to_user(argp, &req, sizeof(req));
		}

		if (ret)
			pr_debug("OVERLAY_GET failed (%d)\n", ret);
		break;

	case MSMFB_OVERLAY_SET:
		ret = copy_from_user(&req, argp, sizeof(req));
		if (!ret) {
			ret = mdss_mdp_overlay_set(mfd, &req);

			if (!IS_ERR_VALUE(ret))
				ret = copy_to_user(argp, &req, sizeof(req));
		}
		if (ret)
			pr_debug("OVERLAY_SET failed (%d)\n", ret);
		break;


	case MSMFB_OVERLAY_UNSET:
		if (!IS_ERR_VALUE(copy_from_user(&val, argp, sizeof(val))))
			ret = mdss_mdp_overlay_unset(mfd, val);
		break;

	case MSMFB_OVERLAY_PLAY_ENABLE:
		if (!copy_from_user(&val, argp, sizeof(val))) {
			mdp5_data->overlay_play_enable = val;
			ret = 0;
		} else {
			pr_err("OVERLAY_PLAY_ENABLE failed (%d)\n", ret);
			ret = -EFAULT;
		}
		break;

	case MSMFB_OVERLAY_PLAY:
		if (mdp5_data->overlay_play_enable) {
			struct msmfb_overlay_data data;

			ret = copy_from_user(&data, argp, sizeof(data));
			if (!ret)
				ret = mdss_mdp_overlay_play(mfd, &data);

			if (ret)
				pr_debug("OVERLAY_PLAY failed (%d)\n", ret);
		} else {
			ret = 0;
		}
		break;

	case MSMFB_OVERLAY_PLAY_WAIT:
		if (mdp5_data->overlay_play_enable) {
			struct msmfb_overlay_data data;

			ret = copy_from_user(&data, argp, sizeof(data));
			if (!ret)
				ret = mdss_mdp_overlay_play_wait(mfd, &data);

			if (ret)
				pr_err("OVERLAY_PLAY_WAIT failed (%d)\n", ret);
		} else {
			ret = 0;
		}
		break;

	case MSMFB_VSYNC_CTRL:
	case MSMFB_OVERLAY_VSYNC_CTRL:
		if (!copy_from_user(&val, argp, sizeof(val))) {
			ret = mdss_mdp_overlay_vsync_ctrl(mfd, val);
		} else {
			pr_err("MSMFB_OVERLAY_VSYNC_CTRL failed (%d)\n", ret);
			ret = -EFAULT;
		}
		break;
	case MSMFB_OVERLAY_COMMIT:
		mdss_fb_wait_for_fence(&(mfd->mdp_sync_pt_data));
		ret = mfd->mdp.kickoff_fnc(mfd);
		break;
	case MSMFB_METADATA_SET:
		ret = copy_from_user(&metadata, argp, sizeof(metadata));
		if (ret)
			return ret;
		ret = mdss_fb_set_metadata(mfd, &metadata);
		break;
	case MSMFB_METADATA_GET:
		ret = copy_from_user(&metadata, argp, sizeof(metadata));
		if (ret)
			return ret;
		ret = mdss_fb_get_metadata(mfd, &metadata);
		if (!ret)
			ret = copy_to_user(argp, &metadata, sizeof(metadata));
		break;
	default:
		if (mfd->panel.type == WRITEBACK_PANEL)
			ret = mdss_mdp_wb_ioctl_handler(mfd, cmd, argp);
		break;
	}

	return ret;
}

static int mdss_mdp_overlay_on(struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_overlay_private *mdp5_data;
	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	mdp5_data = mfd_to_mdp5_data(mfd);
	if (!mdp5_data)
		return -EINVAL;

	if (!mdp5_data->ctl) {
		struct mdss_mdp_ctl *ctl;
		struct mdss_panel_data *pdata;

		pdata = dev_get_platdata(&mfd->pdev->dev);
		if (!pdata) {
			pr_err("no panel connected for fb%d\n", mfd->index);
			return -ENODEV;
		}

		ctl = mdss_mdp_ctl_init(pdata, mfd);
		if (IS_ERR_OR_NULL(ctl)) {
			pr_err("Unable to initialize ctl for fb%d\n",
				mfd->index);
			return PTR_ERR(ctl);
		}
		ctl->vsync_handler.vsync_handler =
						mdss_mdp_overlay_handle_vsync;
		ctl->vsync_handler.cmd_post_flush = false;

		if (mfd->split_display && pdata->next) {
			/* enable split display */
			rc = mdss_mdp_ctl_split_display_setup(ctl, pdata->next);
			if (rc) {
				mdss_mdp_ctl_destroy(ctl);
				return rc;
			}
		}
		mdp5_data->ctl = ctl;
	}

	if (!mfd->panel_info->cont_splash_enabled) {
		rc = mdss_mdp_overlay_start(mfd);
		if (!IS_ERR_VALUE(rc) && (mfd->panel_info->type != DTV_PANEL) &&
			(mfd->panel_info->type != WRITEBACK_PANEL)) {
			rc = mdss_mdp_overlay_kickoff(mfd);

			if (mfd->panel_info->type == MIPI_CMD_PANEL)
				mdss_mdp_display_wait4pingpong(mdp5_data->ctl);
		}
	} else {
		rc = mdss_mdp_ctl_setup(mdp5_data->ctl);
		if (rc)
			return rc;
	}

	if (IS_ERR_VALUE(rc)) {
		pr_err("Failed to turn on fb%d\n", mfd->index);
		mdss_mdp_overlay_off(mfd);
	}
	return rc;
}

static int mdss_mdp_overlay_off(struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_mdp_mixer *mixer;
	int need_cleanup;

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl) {
		pr_err("ctl not initialized\n");
		return -ENODEV;
	}

	if (!mdp5_data->ctl->power_on)
		return 0;

	mdss_mdp_overlay_free_fb_pipe(mfd);

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (mixer)
		mixer->cursor_enabled = 0;

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_RIGHT);
	if (mixer)
		mixer->cursor_enabled = 0;

	mutex_lock(&mfd->lock);
	need_cleanup = !list_empty(&mdp5_data->pipes_cleanup);
	mutex_unlock(&mfd->lock);

	if (need_cleanup) {
		pr_debug("cleaning up pipes on fb%d\n", mfd->index);
		mdss_mdp_overlay_kickoff(mfd);
	}

	/*
	 * If retire fences are still active wait for a vsync time
	 * for retire fence to be updated.
	 * As a last resort signal the timeline if vsync doesn't arrive.
	 */
	if (mdp5_data->retire_cnt) {
		u32 fps = mdss_panel_get_framerate(mfd->panel_info);
		u32 vsync_time = 1000 / (fps ? : DEFAULT_FRAME_RATE);

		msleep(vsync_time);

		__vsync_retire_signal(mfd, mdp5_data->retire_cnt);
	}

	rc = mdss_mdp_ctl_stop(mdp5_data->ctl);
	if (rc == 0) {
		mdss_mdp_ctl_notifier_unregister(mdp5_data->ctl,
				&mfd->mdp_sync_pt_data.notifier);
		if (!mfd->ref_cnt) {
			mdp5_data->borderfill_enable = false;
			mdss_mdp_ctl_destroy(mdp5_data->ctl);
			mdp5_data->ctl = NULL;
		}

		if (atomic_dec_return(&ov_active_panels) == 0)
			mdss_mdp_rotator_release_all();

		rc = pm_runtime_put(&mfd->pdev->dev);
		if (rc)
			pr_err("unable to suspend w/pm_runtime_put (%d)\n", rc);
	}

	return rc;
}

int mdss_panel_register_done(struct mdss_panel_data *pdata)
{
	/*
	 * Clocks are already on if continuous splash is enabled,
	 * increasing ref_cnt to help balance clocks once done.
	 */
	if (pdata->panel_info.cont_splash_enabled) {
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
		mdss_mdp_footswitch_ctrl_splash(1);
		mdss_mdp_ctl_splash_start(pdata);
	}
	return 0;
}

static void __vsync_retire_handle_vsync(struct mdss_mdp_ctl *ctl, ktime_t t)
{
	struct msm_fb_data_type *mfd = ctl->mfd;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd || !mfd->mdp.private1) {
		pr_warn("Invalid handle for vsync\n");
		return;
	}

	mdp5_data = mfd_to_mdp5_data(mfd);
	queue_kthread_work(&mdp5_data->worker, &mdp5_data->vsync_work);
}

static void __vsync_retire_work_handler(struct kthread_work *work)
{
	struct mdss_overlay_private *mdp5_data =
		container_of(work, typeof(*mdp5_data), vsync_work);

	if (!mdp5_data->ctl || !mdp5_data->ctl->mfd)
		return;

	if (!mdp5_data->ctl->remove_vsync_handler)
		return;

	__vsync_retire_signal(mdp5_data->ctl->mfd, 1);
}

static void __vsync_retire_signal(struct msm_fb_data_type *mfd, int val)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	mutex_lock(&mfd->mdp_sync_pt_data.sync_mutex);
	if (mdp5_data->retire_cnt > 0) {
		sw_sync_timeline_inc(mdp5_data->vsync_timeline, val);

		mdp5_data->retire_cnt -= min(val, mdp5_data->retire_cnt);
		if (mdp5_data->retire_cnt == 0) {
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
			mdp5_data->ctl->remove_vsync_handler(mdp5_data->ctl,
					&mdp5_data->vsync_retire_handler);
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		}
	}
	mutex_unlock(&mfd->mdp_sync_pt_data.sync_mutex);
}

static struct sync_fence *
__vsync_retire_get_fence(struct msm_sync_pt_data *sync_pt_data)
{
	struct msm_fb_data_type *mfd;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_mdp_ctl *ctl;
	int rc, value;

	mfd = container_of(sync_pt_data, typeof(*mfd), mdp_sync_pt_data);
	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl)
		return ERR_PTR(-ENODEV);

	ctl = mdp5_data->ctl;
	if (!ctl->add_vsync_handler)
		return ERR_PTR(-EOPNOTSUPP);

	if (!ctl->power_on) {
		pr_debug("fb%d vsync pending first update\n", mfd->index);
		return ERR_PTR(-EPERM);
	}

	if (!mdp5_data->vsync_retire_handler.enabled) {
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
		rc = ctl->add_vsync_handler(ctl,
				&mdp5_data->vsync_retire_handler);
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		if (IS_ERR_VALUE(rc))
			return ERR_PTR(rc);
	}
	value = mdp5_data->vsync_timeline->value + 1 + mdp5_data->retire_cnt;
	mdp5_data->retire_cnt++;

	return mdss_fb_sync_get_fence(mdp5_data->vsync_timeline,
			"mdp-retire", value);
}

static int __vsync_retire_setup(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct sched_param param = { .sched_priority = 5 };
	char name[24];

	snprintf(name, sizeof(name), "mdss_fb%d_retire", mfd->index);
	mdp5_data->vsync_timeline = sw_sync_timeline_create(name);
	if (mdp5_data->vsync_timeline == NULL) {
		pr_err("cannot vsync create time line");
		return -ENOMEM;
	}
	mfd->mdp_sync_pt_data.get_retire_fence = __vsync_retire_get_fence;

	init_kthread_worker(&mdp5_data->worker);
	init_kthread_work(&mdp5_data->vsync_work, __vsync_retire_work_handler);

	mdp5_data->thread = kthread_run(kthread_worker_fn,
				&mdp5_data->worker, "vsync_retire_work");
	if (IS_ERR_OR_NULL(mdp5_data->thread)) {
		pr_err("Unable to start vsync thread\n");
		return -ENOMEM;
	}

	sched_setscheduler(mdp5_data->thread, SCHED_FIFO, &param);

	mdp5_data->vsync_retire_handler.vsync_handler =
		__vsync_retire_handle_vsync;
	mdp5_data->vsync_retire_handler.cmd_post_flush = false;

	return 0;
}

int mdss_mdp_overlay_init(struct msm_fb_data_type *mfd)
{
	struct device *dev = mfd->fbi->dev;
	struct msm_mdp_interface *mdp5_interface = &mfd->mdp;
	struct mdss_overlay_private *mdp5_data = NULL;
	int rc;

	mdp5_interface->on_fnc = mdss_mdp_overlay_on;
	mdp5_interface->off_fnc = mdss_mdp_overlay_off;
	mdp5_interface->release_fnc = __mdss_mdp_overlay_release_all;
	mdp5_interface->do_histogram = NULL;
	mdp5_interface->cursor_update = mdss_mdp_hw_cursor_update;
	mdp5_interface->dma_fnc = mdss_mdp_overlay_pan_display;
	mdp5_interface->ioctl_handler = mdss_mdp_overlay_ioctl_handler;
	mdp5_interface->panel_register_done = mdss_panel_register_done;
	mdp5_interface->kickoff_fnc = mdss_mdp_overlay_kickoff;
	mdp5_interface->get_sync_fnc = mdss_mdp_rotator_sync_pt_get;

	mdp5_data = kmalloc(sizeof(struct mdss_overlay_private), GFP_KERNEL);
	if (!mdp5_data) {
		pr_err("fail to allocate mdp5 private data structure");
		return -ENOMEM;
	}
	memset(mdp5_data, 0, sizeof(struct mdss_overlay_private));

	INIT_LIST_HEAD(&mdp5_data->pipes_used);
	INIT_LIST_HEAD(&mdp5_data->pipes_cleanup);
	INIT_LIST_HEAD(&mdp5_data->rot_proc_list);
	mutex_init(&mdp5_data->ov_lock);
	mdp5_data->hw_refresh = true;
	mdp5_data->overlay_play_enable = true;

	mdp5_data->mdata = dev_get_drvdata(mfd->pdev->dev.parent);
	if (!mdp5_data->mdata) {
		pr_err("unable to initialize overlay for fb%d\n", mfd->index);
		rc = -ENODEV;
		goto init_fail;
	}
	mfd->mdp.private1 = mdp5_data;

	rc = mdss_mdp_overlay_fb_parse_dt(mfd);
	if (rc)
		return rc;

	rc = sysfs_create_group(&dev->kobj, &vsync_fs_attr_group);
	if (rc) {
		pr_err("vsync sysfs group creation failed, ret=%d\n", rc);
		goto init_fail;
	}

	mdp5_data->vsync_event_sd = sysfs_get_dirent(dev->kobj.sd, NULL,
						     "vsync_event");
	if (!mdp5_data->vsync_event_sd) {
		pr_err("vsync_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	rc = sysfs_create_link_nowarn(&dev->kobj,
			&mdp5_data->mdata->pdev->dev.kobj, "mdp");
	if (rc)
		pr_warn("problem creating link to mdp sysfs\n");

	mfd->mdp_sync_pt_data.async_wait_fences = true;
	if (mfd->panel_info->type == MIPI_CMD_PANEL) {
		rc = __vsync_retire_setup(mfd);
		if (IS_ERR_VALUE(rc)) {
			pr_err("unable to create vsync timeline\n");
			goto init_fail;
		}
	}

	pm_runtime_set_suspended(&mfd->pdev->dev);
	pm_runtime_enable(&mfd->pdev->dev);

	kobject_uevent(&dev->kobj, KOBJ_ADD);
	pr_debug("vsync kobject_uevent(KOBJ_ADD)\n");

	mdp5_data->cpu_pm_hdl = add_event_timer(NULL, (void *)mdp5_data);
	if (!mdp5_data->cpu_pm_hdl)
		pr_warn("%s: unable to add event timer\n", __func__);

	return rc;
init_fail:
	kfree(mdp5_data);
	return rc;
}

static int mdss_mdp_overlay_fb_parse_dt(struct msm_fb_data_type *mfd)
{
	struct platform_device *pdev = mfd->pdev;
	struct mdss_overlay_private *mdp5_mdata = mfd_to_mdp5_data(mfd);

	mdp5_mdata->mixer_swap = of_property_read_bool(pdev->dev.of_node,
					   "qcom,mdss-mixer-swap");
	if (mdp5_mdata->mixer_swap) {
		pr_info("mixer swap is enabled for fb device=%s\n",
			pdev->name);
	}

	return 0;
}
