/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Samsung EXYNOS8 SoC series DPP driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/exynos_iovmm.h>
#include <linux/videodev2_exynos_media.h>

#include "dpp.h"
#include "decon.h"

int dpp_log_level = 6;
struct dpp_device *dpp_drvdata[MAX_DPP_CNT];

static int dpp_runtime_suspend(struct device *dev);
static int dpp_runtime_resume(struct device *dev);

static void dpp_dma_dump_registers(struct dpp_device *dpp)
{
	if (dpp->id == ODMA_WB) {
		dpp_info("=== DPU_DMA%d SFR DUMP ===\n", dpp->id);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
				dpp->res.dma_regs, 0xA0, false);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
				dpp->res.dma_regs + 0x300, 0x78, false);

		dpp_info("=== DPU_DMA%d SHADOW SFR DUMP ===\n", dpp->id);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
				dpp->res.dma_regs + 0x800, 0x78, false);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
				dpp->res.dma_regs + 0xB00, 0x78, false);
	} else {
		dpp_info("=== DPU_DMA%d SFR DUMP ===\n", dpp->id);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
				dpp->res.dma_regs, 0x78, false);

		dpp_info("=== DPU_DMA%d SHADOW SFR DUMP ===\n", dpp->id);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
				dpp->res.dma_regs + 0x800, 0x78, false);
	}
}

static void dpp_dump_registers(struct dpp_device *dpp)
{
	dpp_info("=== DPP%d SFR DUMP ===\n", dpp->id);

	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs, 0x58, false);
	if (dpp->id == IDMA_VGF0 || dpp->id == IDMA_VGF1) {
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs + 0x5B0, 0x10, false);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs + 0xA0C, 0x10, false);
	}
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs + 0xA54, 0x4, false);
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs + 0xB00, 0x58, false);
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs + 0xC00, 0x14, false);
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			dpp->res.regs + 0xD00, 0xC, false);
}

void dpp_op_timer_handler(unsigned long arg)
{
	struct dpp_device *dpp = (struct dpp_device *)arg;

	#if !defined(CONFIG_SOC_EXYNOS7872)
	dpp_dma_dump_registers(dpp);
	dpp_dump_registers(dpp);
	#endif

	if (dpp->config->compression)
		dpp_dbg("Compression Source is %s of DPP[%d]\n",
			dpp->config->dpp_parm.comp_src == DPP_COMP_SRC_G2D ?
			"G2D" : "GPU", dpp->id);

	dpp_dbg("DPP[%d] irq hasn't been occured", dpp->id);
}

static int dpp_wb_wait_for_framedone(struct dpp_device *dpp)
{
	int ret;
	int done_cnt;

	if (dpp->id != ODMA_WB) {
		dpp_err("waiting for dpp's framedone is only for writeback\n");
		return -EINVAL;
	}

	if (dpp->state == DPP_STATE_OFF) {
		dpp_err("dpp%d power is off state(%d)\n", dpp->id, dpp->state);
		return -EPERM;
	}

	done_cnt = dpp->d.done_count;
	/* TODO: dma framedone should be wait */
	ret = wait_event_interruptible_timeout(dpp->framedone_wq,
			(done_cnt != dpp->d.done_count), msecs_to_jiffies(17));
	if (ret == 0) {
		dpp_err("timeout of dpp%d framedone\n", dpp->id);
		return -ETIMEDOUT;
	}

	return 0;
}

static void dpp_get_params(struct dpp_device *dpp, struct dpp_params_info *p)
{
	struct decon_win_config *config = dpp->config;

	memcpy(&p->src, &config->src, sizeof(struct decon_frame));
	memcpy(&p->dst, &config->dst, sizeof(struct decon_frame));
	memcpy(&p->block, &config->block_area, sizeof(struct decon_win_rect));
	p->flip = config->dpp_parm.flip;
	p->is_comp = config->compression;
	p->format = config->format;
	p->addr[0] = config->dpp_parm.addr[0];
	p->addr[1] = config->dpp_parm.addr[1];
	p->addr[2] = config->dpp_parm.addr[2];
	p->eq_mode = config->dpp_parm.eq_mode;

	dpp_dbg("dpp%d,p->is_comp : %d, p->format : %d \n", dpp->id, p->is_comp, p->format);
	dpp_dbg("dpp%d,p->src.w : %d, p->src.h : %d \n", dpp->id, p->src.w, p->src.h);

	if (p->format == DECON_PIXEL_FORMAT_NV12N)
		p->addr[1] = NV12N_CBCR_BASE(p->addr[0], p->src.f_w, p->src.f_h);

	p->h_ratio = (p->src.w << 20) / p->dst.w;
	p->v_ratio = (p->src.h << 20) / p->dst.h;

	if ((p->h_ratio != (1 << 20)) || (p->v_ratio != (1 << 20)))
		p->is_scale = true;
	else
		p->is_scale = false;

	if ((config->dpp_parm.flip != DPP_FLIP_NONE) || (p->is_scale) ||
		(p->format >= DECON_PIXEL_FORMAT_NV16) ||
		(p->block.w < BLK_WIDTH_MIN) || (p->block.h < BLK_HEIGHT_MIN))
		p->is_block = false;
	else
		p->is_block = true;
}

static int dpp_check_size(struct dpp_device *dpp, struct dpp_img_format *vi)
{
	struct decon_win_config *config = dpp->config;
	struct decon_frame *src = &config->src;
	struct decon_frame *dst = &config->dst;
	struct dpp_size_constraints vc;

	dpp_constraints_params(&vc, vi);

	if ((!check_align(src->x, src->y, vc.src_mul_x, vc.src_mul_y)) ||
	   (!check_align(src->f_w, src->f_h, vc.src_mul_w, vc.src_mul_h)) ||
	   (!check_align(src->w, src->h, vc.img_mul_w, vc.img_mul_h)) ||
	   (!check_align(dst->w, dst->h, vc.sca_mul_w, vc.sca_mul_h))) {
		dpp_err("Alignment error!\n");
		goto err;
	}

	if (src->w > vc.src_w_max || src->w < vc.src_w_min ||
		src->h > vc.src_h_max || src->h < vc.src_h_min) {
		dpp_err("Unsupported SRC size!\n");
		goto err;
	}

	if (dst->w > vc.sca_w_max || dst->w < vc.sca_w_min ||
		dst->h > vc.sca_h_max || dst->h < vc.sca_h_min) {
		dpp_err("Unsupported DST size!\n");
		goto err;
	}

	return 0;
err:
	dpp_err("offset x : %d, offset y: %d\n", src->x, src->y);
	dpp_err("src_mul_x : %d, src_mul_y : %d\n", vc.src_mul_x, vc.src_mul_y);
	dpp_err("src f_w : %d, src f_h: %d\n", src->f_w, src->f_h);
	dpp_err("src_mul_w : %d, src_mul_h : %d\n", vc.src_mul_w, vc.src_mul_h);
	dpp_err("src w : %d, src h: %d\n", src->w, src->h);
	dpp_err("img_mul_w : %d, img_mul_h : %d\n", vc.img_mul_w, vc.img_mul_h);
	dpp_err("dst w : %d, dst h: %d\n", dst->w, dst->h);
	dpp_err("sca_mul_w : %d, sca_mul_h : %d\n", vc.sca_mul_w, vc.sca_mul_h);
	dpp_err("flip : %d, color_format : %d\n",
				config->dpp_parm.flip, config->format);

	return -EINVAL;
}

static int dpp_check_scale_ratio(struct dpp_params_info *p)
{
	u32 sc_down_max_w, sc_down_max_h;
	u32 sc_up_min_w, sc_up_min_h;

	sc_down_max_w = p->dst.w * 2;
	sc_down_max_h = p->dst.h * 2;
	sc_up_min_w = (p->dst.w + 7) / 8;
	sc_up_min_h = (p->dst.h + 7) / 8;

	if (p->src.w > sc_down_max_w || p->src.h > sc_down_max_h) {
		dpp_err("Not support under 1/2x scale-down!\n");
		goto err;
	}

	if (p->src.w < sc_up_min_w || p->src.h < sc_up_min_h) {
		dpp_err("Not support over 8x scale-up\n");
		goto err;
	}

	return 0;
err:
	dpp_err("src w(%d) h(%d), dst w(%d) h(%d), flip(%d)\n",
			p->src.w, p->src.h, p->dst.w, p->dst.h, p->flip);
	return -EINVAL;
}

static int dpp_check_format(struct dpp_device *dpp, struct dpp_params_info *p)
{
	if ((dpp->id == IDMA_G0 || dpp->id == IDMA_G1 || dpp->id == IDMA_GF) &&
			(p->format >= DECON_PIXEL_FORMAT_NV16)) {
		dpp_err("Not support YUV format(%d) in DPP%d - VG & VGF only!\n",
			p->format, dpp->id);
		return -EINVAL;
	}

	if (dpp->id != IDMA_VGF0 && dpp->id != IDMA_VGF1) {
		if (p->is_comp) {
			dpp_err("Not support AFBC decoding in DPP%d - VGF only!\n",
				dpp->id);
			return -EINVAL;
		}

		if (p->is_scale) {
			dpp_err("Not support SCALING in DPP%d - VGF only!\n", dpp->id);
			return -EINVAL;
		}
	}

	switch (p->format) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
	case DECON_PIXEL_FORMAT_RGB_565:
	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_NV12N:
		break;
	default:
		dpp_err("Unsupported Format\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * TODO: h/w limitation will be changed in KC
 * This function must be modified for KC after releasing DPP constraints
 */
static int dpp_check_limitation(struct dpp_device *dpp, struct dpp_params_info *p)
{
	int ret;
	struct dpp_img_format vi;

	ret = dpp_check_scale_ratio(p);
	if (ret) {
		dpp_err("failed to set dpp%d scale information\n", dpp->id);
		return -EINVAL;
	}

	dpp_select_format(dpp, &vi, p);

	ret = dpp_check_format(dpp, p);
	if (ret)
		return -EINVAL;

	if (p->is_comp && p->flip) {
		dpp_err("Not support [AFBC+FLIP] at the same time in DPP%d\n",
			dpp->id);
		return -EINVAL;
	}

	if (p->is_comp && p->is_block) {
		dpp_err("Not support [AFBC+BLOCK] at the same time in DPP%d\n",
			dpp->id);
		return -EINVAL;
	}

	if (p->is_comp && vi.yuv420) {
		dpp_err("Not support AFBC decoding for YUV format in DPP%d\n",
			dpp->id);
		return -EINVAL;
	}

	if (p->is_block && p->is_scale) {
		dpp_err("Not support [BLOCK+SCALE] at the same time in DPP%d\n",
			dpp->id);
		return -EINVAL;
	}

	if (p->is_block && vi.yuv420) {
		dpp_err("Not support BLOCK Mode for YUV format in DPP%d\n",
			dpp->id);
		return -EINVAL;
	}

	/* FIXME */
	if (p->is_block && p->flip) {
		dpp_err("Not support [BLOCK+FLIP] at the same time in DPP%d\n",
			dpp->id);
		return -EINVAL;
	}

	ret = dpp_check_size(dpp, &vi);
	if (ret)
		return -EINVAL;

	return 0;
}

static int dpp_set_config(struct dpp_device *dpp)
{
	struct dpp_params_info params;
	int ret = 0;

	mutex_lock(&dpp->lock);

	if (dpp->state == DPP_STATE_OFF) {
		dpp_dbg("dpp%d is started\n", dpp->id);
#if defined(CONFIG_PM)
		pm_runtime_get_sync(dpp->dev);
#else
		dpp_runtime_resume(dpp->dev);
#endif
		dpp_reg_init(dpp->id);

		enable_irq(dpp->res.dma_irq);
		if (dpp->id != ODMA_WB) {
			enable_irq(dpp->res.irq);
		}

	}

	/* parameters from decon driver are translated for dpp driver */
	dpp_get_params(dpp, &params);

	/* all parameters must be passed dpp hw limitation */
	ret = dpp_check_limitation(dpp, &params);
	if (ret)
		goto err;

	/* set all parameters to dpp hw */
	dpp_reg_configure_params(dpp->id, &params);

	dpp->d.op_timer.expires = (jiffies + 1 * HZ);
	mod_timer(&dpp->d.op_timer, dpp->d.op_timer.expires);

	DPU_EVENT_LOG(DPU_EVT_DPP_WINCON, &dpp->sd, ktime_set(0, 0));

	/*
	 * It's only for DPP BIST mode test
	 * dma_reg_set_ch_map(0, dpp->id, true);
	 * dma_reg_set_test_pattern(0, 0, pat_dat[0]);
	 * dma_reg_set_test_pattern(0, 1, pat_dat[1]);
	 */

	dpp->state = DPP_STATE_ON;
err:
	mutex_unlock(&dpp->lock);
	dpp_dbg("dpp%d is finished \n", dpp->id);
	return ret;
}

static int dpp_stop(struct dpp_device *dpp, bool reset)
{
	int ret = 0;

	mutex_lock(&dpp->lock);

	if (dpp->state == DPP_STATE_OFF) {
		dpp_warn("dpp%d is already disabled\n", dpp->id);
		goto err;
	}

	DPU_EVENT_LOG(DPU_EVT_DPP_STOP, &dpp->sd, ktime_set(0, 0));

	disable_irq(dpp->res.dma_irq);
	if (dpp->id != ODMA_WB) {
		disable_irq(dpp->res.irq);
	}

	del_timer(&dpp->d.op_timer);
	dpp_reg_deinit(dpp->id, reset);

#if defined(CONFIG_PM)
	pm_runtime_put_sync(dpp->dev);
#else
	dpp_runtime_suspend(dpp->dev);
#endif
	dpp_dbg("dpp%d is stopped\n", dpp->id);

	dpp->state = DPP_STATE_OFF;
err:
	mutex_unlock(&dpp->lock);
	return ret;
}

static int dpp_s_stream(struct v4l2_subdev *sd, int enable)
{
	dpp_dbg("%s: subdev name(%s)\n", __func__, sd->name);
	return 0;
}

static long dpp_subdev_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct dpp_device *dpp = v4l2_get_subdevdata(sd);
	bool reset = (bool)arg;
	unsigned long val;
	int ret = 0;

	switch (cmd) {
	case DPP_WIN_CONFIG:
		dpp->config = (struct decon_win_config *)arg;
		ret = dpp_set_config(dpp);
		if (ret)
			dpp_err("failed to configure dpp%d\n", dpp->id);
		break;

	case DPP_STOP:
		ret = dpp_stop(dpp, reset);
		if (ret)
			dpp_err("failed to stop dpp%d\n", dpp->id);
		break;

	case DPP_DUMP:
		dpp_dma_dump_registers(dpp);
		dpp_dump_registers(dpp);
		break;

	case DPP_WB_WAIT_FOR_FRAMEDONE:
		ret = dpp_wb_wait_for_framedone(dpp);
		break;

	case DPP_WAIT_IDLE:
		val = (unsigned long)arg;
		if (dpp->state == DPP_STATE_ON)
			dpp_reg_wait_idle_status(dpp->id, val);
		break;

	default:
		break;
	}

	return ret;
}

static const struct v4l2_subdev_core_ops dpp_subdev_core_ops = {
	.ioctl = dpp_subdev_ioctl,
};

static const struct v4l2_subdev_video_ops dpp_subdev_video_ops = {
	.s_stream = dpp_s_stream,
};

static struct v4l2_subdev_ops dpp_subdev_ops = {
	.core = &dpp_subdev_core_ops,
	.video = &dpp_subdev_video_ops,
};

static void dpp_init_subdev(struct dpp_device *dpp)
{
	struct v4l2_subdev *sd = &dpp->sd;

	v4l2_subdev_init(sd, &dpp_subdev_ops);
	sd->owner = THIS_MODULE;
	sd->grp_id = dpp->id;
	snprintf(sd->name, sizeof(sd->name), "%s.%d", "dpp-sd", dpp->id);
	v4l2_set_subdevdata(sd, dpp);
}

#if defined(CONFIG_SOC_EXYNOS8895) || defined(CONFIG_SOC_EXYNOS7872)
static irqreturn_t dpp_irq_handler(int irq, void *priv)
{
	struct dpp_device *dpp = priv;
	u32 dpp_irq = 0;

	spin_lock(&dpp->slock);
	if (dpp->state == DPP_STATE_OFF)
		goto irq_end;

	dpp_irq = dpp_reg_get_irq_status(dpp->id);
	dpp_reg_clear_irq(dpp->id, dpp_irq);

	if (dpp_irq & DPP_CONFIG_ERROR) {
		dpp_err("dpp%d config error irq occur(0x%x)\n",
				dpp->id, dpp_irq);
		dpp_err("DPP_CFG_ERR_STATE(0x%x)\n",
				dpp_read(dpp->id, DPP_CFG_ERR_STATE));
		dpp_dump_registers(dpp);
		goto irq_end;
	}

irq_end:
	del_timer(&dpp->d.op_timer);
	spin_unlock(&dpp->slock);
	return IRQ_HANDLED;
}

static irqreturn_t dma_irq_handler(int irq, void *priv)
{
	struct dpp_device *dpp = priv;
	u32 irqs = 0;
	u32 reg_id = 0;
	u32 cfg_err = 0;
	u32 irq_pend = 0;
	u32 val = 0;

	spin_lock(&dpp->dma_slock);
	if (dpp->state == DPP_STATE_OFF)
		goto irq_end;

	irqs = dma_reg_get_irq_status(dpp->id);
	/* CFG_ERR_STATE SFR is cleared when clearing pending bits */
	if (dpp->id == ODMA_WB) {
		reg_id = ODMA_CFG_ERR_STATE;
		irq_pend = ODMA_CONFIG_ERROR;
	} else {
		reg_id = IDMA_CFG_ERR_STATE;
		irq_pend = IDMA_CONFIG_ERROR;
	}
	if (irqs & irq_pend)
		cfg_err = dma_read(dpp->id, reg_id);
	dma_reg_clear_irq(dpp->id, irqs);

	if (dpp->id == ODMA_WB) {
		if (irqs & ODMA_CONFIG_ERROR) {
			dpp_err("dma%d config error occur(0x%x)\n", dpp->id, irqs);
			dpp_err("CFG_ERR_STATE = (0x%x)\n", cfg_err);
			/* TODO: add to read config error information */
			dpp_dump_registers(dpp);
			goto irq_end;
		}

		if ((irqs & ODMA_WRITE_SLAVE_ERROR) ||
			       (irqs & ODMA_STATUS_DEADLOCK_IRQ)) {
			dpp_err("dma%d error irq occur(0x%x)\n", dpp->id, irqs);
			dpp_dump_registers(dpp);
			goto irq_end;
		}

		if (irqs & ODMA_STATUS_FRAMEDONE_IRQ) {
			dpp->d.done_count++;
			if (dpp->id == ODMA_WB)
				wake_up_interruptible_all(&dpp->framedone_wq);
			DPU_EVENT_LOG(DPU_EVT_DPP_FRAMEDONE, &dpp->sd,
					ktime_set(0, 0));
			goto irq_end;
		}
	} else {
		if (irqs & IDMA_RECOVERY_START_IRQ) {
			DPU_EVENT_LOG(DPU_EVT_DMA_RECOVERY, &dpp->sd,
					ktime_set(0, 0));
			val = (u32)dpp->config->dpp_parm.comp_src;
			dpp_info("dma%d recovery start(0x%x)...[src=%s]\n",
				dpp->id, irqs,
				val == DPP_COMP_SRC_G2D ? "G2D" : "GPU");
			goto irq_end;
		}

		if ((irqs & IDMA_AFBC_TIMEOUT_IRQ) ||
				(irqs & IDMA_READ_SLAVE_ERROR) ||
				(irqs & IDMA_STATUS_DEADLOCK_IRQ)) {
			dpp_err("dma%d error irq occur(0x%x)\n", dpp->id, irqs);
			dpp_dump_registers(dpp);
			goto irq_end;
		}

		if (irqs & IDMA_CONFIG_ERROR) {
			val = IDMA_CFG_ERR_IMG_HEIGHT
				| IDMA_CFG_ERR_IMG_HEIGHT_ROTATION;
			if (cfg_err & val)
				dpp_err("dma%d config: img_height(0x%x)\n",
					dpp->id, irqs);
			else {
				dpp_err("dma%d config error occur(0x%x)\n",
					dpp->id, irqs);
				dpp_err("CFG_ERR_STATE = (0x%x)\n", cfg_err);
				/* TODO: add to read config error information */
				/*
				 * Disabled because this can cause slow update
				 * if conditions happen very often
				 *	dpp_dump_registers(dpp);
				*/
			}
			goto irq_end;
		}

		if (irqs & IDMA_STATUS_FRAMEDONE_IRQ) {
			/*
			 * TODO: Normally, DMA framedone occurs before
			 * DPP framedone. But DMA framedone can occur in case
			 * of AFBC crop mode
			 */
			DPU_EVENT_LOG(DPU_EVT_DMA_FRAMEDONE, &dpp->sd, ktime_set(0, 0));
			goto irq_end;
		}
	}

irq_end:
	spin_unlock(&dpp->dma_slock);
	return IRQ_HANDLED;
}
#else
static irqreturn_t dpp_irq_handler(int irq, void *priv)
{
	struct dpp_device *dpp = priv;
	u32 dpp_irq = 0;

	spin_lock(&dpp->slock);
	if (dpp->state == DPP_STATE_OFF)
		goto irq_end;

	dpp_irq = dpp_reg_get_irq_status(dpp->id);
	dpp_reg_clear_irq(dpp->id, dpp_irq);

	if ((dpp_irq & VG_IRQ_DEADLOCK_STATUS) ||
			(dpp_irq & VG_IRQ_READ_SLAVE_ERROR)) {
		dpp_err("dpp%d error irq occur(0x%x)\n", dpp->id, dpp_irq);
		dpp_dump_registers(dpp);
		exynos_sysmmu_show_status(dpp->dev);
		goto irq_end;
	}

	if (dpp_irq & VG_IRQ_FRAMEDONE) {
		dpp->d.done_count++;
		if (dpp->id == ODMA_WB)
			wake_up_interruptible_all(&dpp->framedone_wq);
		DPU_EVENT_LOG(DPU_EVT_DPP_FRAMEDONE, &dpp->sd, ktime_set(0, 0));
	}

irq_end:
	del_timer(&dpp->d.op_timer);
	spin_unlock(&dpp->slock);
	return IRQ_HANDLED;
}
#endif

static int dpp_get_clocks(struct dpp_device *dpp)
{
#if !defined(CONFIG_SOC_EXYNOS8895) && !defined(CONFIG_SOC_EXYNOS7872)
	struct device *dev = dpp->dev;

	dpp->res.gate = devm_clk_get(dev, "dpp_clk");
	if (IS_ERR_OR_NULL(dpp->res.gate)) {
		dpp_err("failed to get dpp%d clock\n", dpp->id);
		return PTR_ERR(dpp->res.gate);
	}
#endif
	return 0;
}

#if 0
static int dpp_sysmmu_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long iova, int flags, void *token)
{
	struct dpp_device *dpp = dev_get_drvdata(dev);

	if (dpp->state == DPP_STATE_ON) {
		dpp_info("dpp%d sysmmu fault handler\n", dpp->id);
		dpp_dump_registers(dpp);
	}

	return 0;
}
#endif

static void dpp_parse_dt(struct dpp_device *dpp, struct device *dev)
{
	dpp->id = of_alias_get_id(dev->of_node, "dpp");
	dpp_info("dpp(%d) probe start..\n", dpp->id);

	dpp->dev = dev;
}

static int dpp_init_resources(struct dpp_device *dpp, struct platform_device *pdev)
{
	struct resource *res;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dpp_err("failed to get mem resource\n");
		return -ENOENT;
	}
	dpp_info("res: start(0x%x), end(0x%x)\n", (u32)res->start, (u32)res->end);

	dpp->res.regs = devm_ioremap_resource(dpp->dev, res);
	if (!dpp->res.regs) {
		dpp_err("failed to remap DPP SFR region\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dpp_err("failed to get mem resource\n");
		return -ENOENT;
	}
	dpp_info("dma res: start(0x%x), end(0x%x)\n", (u32)res->start, (u32)res->end);

	dpp->res.dma_regs = devm_ioremap_resource(dpp->dev, res);
	if (!dpp->res.dma_regs) {
		dpp_err("failed to remap DPU_DMA SFR region\n");
		return -EINVAL;
	}

	if (dpp->id == IDMA_G0) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		if (!res) {
			dpp_err("failed to get mem resource\n");
			return -ENOENT;
		}
		dpp_info("dma common res: start(0x%x), end(0x%x)\n",
				(u32)res->start, (u32)res->end);
		dpp->res.dma_com_regs = devm_ioremap_resource(dpp->dev, res);
		if (!dpp->res.dma_com_regs) {
			dpp_err("failed to remap DPU_DMA COMMON SFR region\n");
			return -EINVAL;
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dpp_err("failed to get dpu dma irq resource\n");
		return -ENOENT;
	}
	dpp_info("irq no = %lld\n", res->start);

	dpp->res.dma_irq = res->start;
	ret = devm_request_irq(dpp->dev, res->start, dma_irq_handler, 0,
			pdev->name, dpp);
	if (ret) {
		dpp_err("failed to install DPU DMA irq\n");
		return -EINVAL;
	}
	disable_irq(dpp->res.dma_irq);
#if defined(CONFIG_SOC_EXYNOS8895) || defined(CONFIG_SOC_EXYNOS7872)
	if (dpp->id != ODMA_WB && dpp->id == IDMA_VG0) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, 1);
		if (!res) {
			dpp_err("failed to get dpp irq resource\n");
			return -ENOENT;
		}
		dpp_info("dpp irq no = %lld\n", res->start);

		dpp->res.irq = res->start;
		ret = devm_request_irq(dpp->dev, res->start, dpp_irq_handler, 0,
				pdev->name, dpp);
		if (ret) {
			dpp_err("failed to install DPP irq\n");
			return -EINVAL;
		}
		disable_irq(dpp->res.irq);
	}
#endif

	return 0;
}

static int dpp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dpp_device *dpp;
	int ret = 0;

	dpp = devm_kzalloc(dev, sizeof(*dpp), GFP_KERNEL);
	if (!dpp) {
		dpp_err("failed to allocate dpp device.\n");
		ret = -ENOMEM;
		goto err;
	}
	dpp_parse_dt(dpp, dev);

	dpp_drvdata[dpp->id] = dpp;
	ret = dpp_get_clocks(dpp);
	if (ret)
		goto err_clk;

	spin_lock_init(&dpp->slock);
	spin_lock_init(&dpp->dma_slock);
	mutex_init(&dpp->lock);
	init_waitqueue_head(&dpp->framedone_wq);

	ret = dpp_init_resources(dpp, pdev);
	if (ret)
		goto err_clk;

	dpp_init_subdev(dpp);
	platform_set_drvdata(pdev, dpp);

	setup_timer(&dpp->d.op_timer, dpp_op_timer_handler, (unsigned long)dpp);

	pm_runtime_enable(dev);

	ret = iovmm_activate(dev);
	if (ret) {
		dpp_err("failed to activate iovmm\n");
		goto err_clk;
	}

	/* TODO: iommu fault handler will be added in the future */
	/* iovmm_set_fault_handler(dev, dpp_sysmmu_fault_handler, NULL); */

	dpp->state = DPP_STATE_OFF;
	dpp_info("dpp%d is probed successfully\n", dpp->id);

	return 0;

err_clk:
	kfree(dpp);
err:
	return ret;
}

static int dpp_remove(struct platform_device *pdev)
{

	struct dpp_device *dpp = platform_get_drvdata(pdev);

	iovmm_deactivate(dpp->dev);
	dpp_dbg("%s driver unloaded\n", pdev->name);

	return 0;
}

static int dpp_runtime_suspend(struct device *dev)
{
	return 0;
}

static int dpp_runtime_resume(struct device *dev)
{
	return 0;
}

static const struct of_device_id dpp_of_match[] = {
	{ .compatible = "samsung,exynos7872-dpp" },
	{},
};
MODULE_DEVICE_TABLE(of, dpp_of_match);

static const struct dev_pm_ops dpp_pm_ops = {
	.runtime_suspend	= dpp_runtime_suspend,
	.runtime_resume		= dpp_runtime_resume,
};

static struct platform_driver dpp_driver __refdata = {
	.probe		= dpp_probe,
	.remove		= dpp_remove,
	.driver = {
		.name	= DPP_MODULE_NAME,
		.owner	= THIS_MODULE,
		.pm	= &dpp_pm_ops,
		.of_match_table = of_match_ptr(dpp_of_match),
	}
};

static int dpp_register(void)
{
	return platform_driver_register(&dpp_driver);
}

static void dpp_unregister(void)
{
	return platform_driver_unregister(&dpp_driver);
}

device_initcall_sync(dpp_register);
module_exit(dpp_unregister);

MODULE_AUTHOR("Jaehoe Yang <jaehoe.yang@samsung.com>");
MODULE_AUTHOR("Minho Kim <m8891.kim@samsung.com>");
MODULE_AUTHOR("SeungBeom Park <sb1.park@samsung.com>");
MODULE_DESCRIPTION("Samsung EXYNOS DPP driver");
MODULE_LICENSE("GPL");
