// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <soc/mediatek/smi.h>
#include <linux/slab.h>
#include "smi_public.h"
#include "mtk_vcodec_dec_pm.h"
#include "mtk_vcodec_util.h"
#include "mtk_vcu.h"

#if DEC_DVFS
#include <linux/pm_qos.h>
#include <linux/soc/mediatek/mtk-pm-qos.h>
#include <mmdvfs_pmqos.h>
#include "vcodec_dvfs.h"
#define STD_VDEC_FREQ 228
static struct mtk_pm_qos_request vdec_qos_req_f;
static u64 vdec_freq;
static u32 vdec_freq_step_size;
static u64 vdec_freq_steps[MAX_FREQ_STEP];
static struct codec_history *vdec_hists;
static struct codec_job *vdec_jobs;
#endif

#if DEC_EMI_BW
#include <mtk_smi.h>
static unsigned int h264_frm_scale[4] = {12, 24, 40, 12};
static unsigned int h265_frm_scale[4] = {12, 24, 40, 12};
static unsigned int vp9_frm_scale[4] = {12, 24, 40, 12};
static unsigned int vp8_frm_scale[4] = {12, 24, 40, 12};
static unsigned int mp24_frm_scale[5] = {16, 20, 32, 50, 16};

struct mtk_pm_qos_request vdec_qos_req_bw;
#endif

void mtk_dec_init_ctx_pm(struct mtk_vcodec_ctx *ctx)
{
	ctx->input_driven = 0;
	ctx->user_lock_hw = 1;
}

int mtk_vcodec_init_dec_pm(struct mtk_vcodec_dev *mtkdev)
{
	int ret = 0;
#ifndef FPGA_PWRCLK_API_DISABLE
	struct device_node *node;
	struct platform_device *pdev;
	struct mtk_vcodec_pm *pm;

	pdev = mtkdev->plat_dev;
	pm = &mtkdev->pm;
	pm->mtkdev = mtkdev;
	pm->chip_node = of_find_compatible_node(NULL,
		NULL, "mediatek,vdec_gcon");
	node = of_parse_phandle(pdev->dev.of_node, "mediatek,larb", 0);
	if (!node) {
		mtk_v4l2_err("of_parse_phandle mediatek,larb fail!");
		return -1;
	}

	pdev = of_find_device_by_node(node);
	if (WARN_ON(!pdev)) {
		of_node_put(node);
		return -1;
	}
	pm->larbvdec = &pdev->dev;
	pdev = mtkdev->plat_dev;
	pm->dev = &pdev->dev;

	if (pm->chip_node) {

		pm->clk_MT_SCP_SYS_VDE =
			devm_clk_get(&pdev->dev, "MT_SCP_SYS_VDE");
		if (IS_ERR(pm->clk_MT_SCP_SYS_VDE)) {
			mtk_v4l2_err("[VCODEC][ERROR] Unable to devm_clk_get MT_SCP_SYS_VDE\n");
			return PTR_ERR(pm->clk_MT_SCP_SYS_VDE);
		}
		pm->clk_MT_SCP_SYS_DIS =
			devm_clk_get(&pdev->dev, "MT_SCP_SYS_DIS");
		if (IS_ERR(pm->clk_MT_SCP_SYS_DIS)) {
			mtk_v4l2_err("[VCODEC][ERROR] Unable to devm_clk_get MT_SCP_SYS_DIS\n");
			return PTR_ERR(pm->clk_MT_SCP_SYS_DIS);
		}

		pm->clk_MT_CG_VDEC = devm_clk_get(&pdev->dev, "MT_CG_VDEC");
		if (IS_ERR(pm->clk_MT_CG_VDEC)) {
			mtk_v4l2_err("[VCODEC][ERROR] Unable to devm_clk_get MT_CG_VDEC\n");
			return PTR_ERR(pm->clk_MT_CG_VDEC);
		}
	} else
		mtk_v4l2_err("[VCODEC][ERROR] DTS went wrong...");
#endif
	return ret;
}

void mtk_vcodec_release_dec_pm(struct mtk_vcodec_dev *dev)
{
#if DEC_DVFS
	mutex_lock(&dev->dec_dvfs_mutex);
	free_hist(&vdec_hists, 0);
	mutex_unlock(&dev->dec_dvfs_mutex);
#endif
}

void mtk_vcodec_dec_pw_on(struct mtk_vcodec_pm *pm, int hw_id)
{
}

void mtk_vcodec_dec_pw_off(struct mtk_vcodec_pm *pm, int hw_id)
{
}

void mtk_vcodec_dec_clock_on(struct mtk_vcodec_pm *pm, int hw_id)
{
#ifndef FPGA_PWRCLK_API_DISABLE
	int ret;

	smi_bus_prepare_enable(SMI_LARB1, "VDEC");
	ret = clk_prepare_enable(pm->clk_MT_CG_VDEC);
	if (ret)
		mtk_v4l2_err("clk_prepare_enable CG_VDEC fail %d", ret);
#endif
}

void mtk_vcodec_dec_clock_off(struct mtk_vcodec_pm *pm, int hw_id)
{
#ifndef FPGA_PWRCLK_API_DISABLE
	struct mtk_vcodec_dev *dev;

	dev = container_of(pm, struct mtk_vcodec_dev, pm);
	mtk_vdec_hw_break(dev, hw_id);

	clk_disable_unprepare(pm->clk_MT_CG_VDEC);
	smi_bus_disable_unprepare(SMI_LARB1, "VDEC");
#endif
}

void mtk_vdec_hw_break(struct mtk_vcodec_dev *dev, int hw_id)
{
	u32 cg_status = 0;
	void __iomem *vdec_misc_addr = dev->dec_reg_base[VDEC_MISC];
	void __iomem *vdec_vld_addr = dev->dec_reg_base[VDEC_VLD];
	void __iomem *vdec_gcon_addr = dev->dec_reg_base[VDEC_SYS];
	struct mtk_vcodec_ctx *ctx = NULL;
	int misc_offset[4] = {64, 66, 67, 65};

	struct timeval tv_start;
	struct timeval tv_end;
	s32 usec, timeout = 20000;
	int offset, idx;
	unsigned long value;
	u32 fourcc;

	if (hw_id == MTK_VDEC_CORE) {
		ctx = dev->curr_dec_ctx[hw_id];
		if (ctx)
			fourcc = ctx->q_data[MTK_Q_DATA_SRC].fmt->fourcc;
		else
			fourcc = 0;
		if (readl(vdec_gcon_addr) == 0) {
			mtk_v4l2_debug(0, "VDEC not HW break since clk off. codec:0x%08x(%c%c%c%c)",
			    fourcc, fourcc & 0xFF, (fourcc >> 8) & 0xFF,
			    (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF);
			return;
		}
		/* hw break */
		writel((readl(vdec_misc_addr + 0x0100) | 0x1),
			vdec_misc_addr + 0x0100);

		do_gettimeofday(&tv_start);
		cg_status = readl(vdec_misc_addr + 0x0104);
		while (!((cg_status & 0x1) && (cg_status & 0x10))) {
			do_gettimeofday(&tv_end);
			usec = (tv_end.tv_sec - tv_start.tv_sec) * 1000000 +
			       tv_end.tv_usec - tv_start.tv_usec;
			if (usec > timeout) {
				mtk_v4l2_err("VDEC HW break timeout. codec:0x%08x(%c%c%c%c)",
				    fourcc, fourcc & 0xFF, (fourcc >> 8) & 0xFF,
				    (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF);
				value = readl(vdec_gcon_addr + (0 << 2));
				mtk_v4l2_err("[DEBUG][GCON] 0x%x(%d) = 0x%lx",
					0 << 2, 0, value);
				value = readl(vdec_gcon_addr + (6 << 2));
				mtk_v4l2_err("[DEBUG][GCON] 0x%x(%d) = 0x%lx",
					6 << 2, 6, value);
				for (offset = 64; offset <= 79; offset++) {
					value = readl(
					    vdec_misc_addr + (offset << 2));
					mtk_v4l2_err("[DEBUG][MISC] 0x%x(%d) = 0x%lx",
						offset << 2, offset, value);
				}
				for (idx = 0; idx < 4; idx++) {
					offset = misc_offset[idx];
					value = readl(
					    vdec_misc_addr + (offset << 2));
					mtk_v4l2_err("[DEBUG][MISC] 0x%x(%d) = 0x%lx",
						offset << 2, offset, value);
				}

				if (timeout == 20000)
					timeout = 1000000;
				else if (timeout == 1000000) {
					/* v4l2_aee_print(
					 *    "%s %p codec:0x%08x(%c%c%c%c) hw break timeout\n",
					 *    __func__, ctx, fourcc,
					 *    fourcc & 0xFF, (fourcc >> 8) & 0xFF,
					 *    (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF);
					 */
					break;
				}
				do_gettimeofday(&tv_start);
				//smi_debug_bus_hang_detect(0, "VCODEC");
			}
			cg_status = readl(vdec_misc_addr + 0x0104);
		}

		/* sw reset */
		writel(0x1, vdec_vld_addr + 0x0108);
		writel(0x0, vdec_vld_addr + 0x0108);
	} else {
		mtk_v4l2_err("hw_id (%d) is unknown\n", hw_id);
	}
}

void mtk_prepare_vdec_dvfs(void)
{
#if DEC_DVFS
	int ret;

	mtk_pm_qos_add_request(&vdec_qos_req_f, PM_QOS_VDEC_FREQ,
				PM_QOS_DEFAULT_VALUE);
	vdec_freq_step_size = 1;
	ret = mmdvfs_qos_get_freq_steps(PM_QOS_VDEC_FREQ, &vdec_freq_steps[0],
					&vdec_freq_step_size);
	if (ret < 0)
		pr_debug("Failed to get vdec freq steps (%d)\n", ret);
#endif
}

void mtk_unprepare_vdec_dvfs(void)
{
#if DEC_DVFS
	int freq_idx = 0;

	freq_idx = (vdec_freq_step_size == 0) ? 0 : (vdec_freq_step_size - 1);
	mtk_pm_qos_update_request(&vdec_qos_req_f, vdec_freq_steps[freq_idx]);
	mtk_pm_qos_remove_request(&vdec_qos_req_f);
	free_hist(&vdec_hists, 0);
	/* TODO: jobs error handle */
#endif
}

void mtk_prepare_vdec_emi_bw(void)
{
#if DEC_EMI_BW
	mtk_pm_qos_add_request(&vdec_qos_req_bw, MTK_PM_QOS_MM_MEMORY_BANDWIDTH,
						PM_QOS_DEFAULT_VALUE);
#endif
}

void mtk_unprepare_vdec_emi_bw(void)
{
#if DEC_EMI_BW
	mtk_pm_qos_remove_request(&vdec_qos_req_bw);
#endif
}

void mtk_vdec_dvfs_begin(struct mtk_vcodec_ctx *ctx)
{
#if DEC_DVFS
	int target_freq = 0;
	u64 target_freq_64 = 0;
	struct codec_job *vdec_cur_job = 0;

	mutex_lock(&ctx->dev->dec_dvfs_mutex);
	vdec_cur_job = move_job_to_head(&ctx->id, &vdec_jobs);
	if (vdec_cur_job != 0) {
		vdec_cur_job->start = get_time_us();
		target_freq = est_freq(vdec_cur_job->handle, &vdec_jobs,
					vdec_hists);
		target_freq_64 = match_freq(target_freq, &vdec_freq_steps[0],
					vdec_freq_step_size);
		if (target_freq > 0) {
			vdec_freq = target_freq;
			if (vdec_freq > target_freq_64)
				vdec_freq = target_freq_64;
			vdec_cur_job->mhz = (int)target_freq_64;
			mtk_pm_qos_update_request(&vdec_qos_req_f, target_freq_64);
		}
	} else {
		target_freq_64 = match_freq(DEFAULT_MHZ, &vdec_freq_steps[0],
						vdec_freq_step_size);
		mtk_pm_qos_update_request(&vdec_qos_req_f, target_freq_64);
	}
	mutex_unlock(&ctx->dev->dec_dvfs_mutex);
#endif
}

void mtk_vdec_dvfs_end(struct mtk_vcodec_ctx *ctx)
{
#if DEC_DVFS
	int freq_idx = 0;
	struct codec_job *vdec_cur_job = 0;

	/* vdec dvfs */
	mutex_lock(&ctx->dev->dec_dvfs_mutex);
	vdec_cur_job = vdec_jobs;
	if (vdec_cur_job != 0 && vdec_cur_job->handle == &ctx->id) {
		vdec_cur_job->end = get_time_us();
		update_hist(vdec_cur_job, &vdec_hists, 0);
		vdec_jobs = vdec_jobs->next;
		kfree(vdec_cur_job);
	} else {
		/* print error log */
	}

	freq_idx = (vdec_freq_step_size == 0) ? 0 : (vdec_freq_step_size - 1);
	mtk_pm_qos_update_request(&vdec_qos_req_f, vdec_freq_steps[freq_idx]);
	mutex_unlock(&ctx->dev->dec_dvfs_mutex);
#endif
}

void mtk_vdec_emi_bw_begin(struct mtk_vcodec_ctx *ctx)
{
#if DEC_EMI_BW
	int f_type = 1; /* TODO */
	long emi_bw = 0;
	long emi_bw_output = 0;

	if (vdec_freq_step_size > 1)
		vdec_freq_step_size - 1;

	emi_bw = 8L * 1920 * 1080 * 3 * 10 * vdec_freq;
	8 * vdec_freq / STD_VDEC_FREQ;
	1920 * 1088 * 3 * 30 * 10 * vdec_freq /
			2 / 3 / STD_VDEC_FREQ / 1024 / 1024;

	switch (ctx->q_data[MTK_Q_DATA_SRC].fmt->fourcc) {
	case V4L2_PIX_FMT_H264:
		emi_bw = emi_bw * h264_frm_scale[f_type] / (2 * STD_VDEC_FREQ);
		break;
	case V4L2_PIX_FMT_H265:
		emi_bw = emi_bw * h265_frm_scale[f_type] / (2 * STD_VDEC_FREQ);
		break;
	case V4L2_PIX_FMT_VP8:
		emi_bw = emi_bw * vp8_frm_scale[f_type] / (2 * STD_VDEC_FREQ);
		break;
	case V4L2_PIX_FMT_VP9:
		emi_bw = emi_bw * vp9_frm_scale[f_type] / (2 * STD_VDEC_FREQ);
		break;
	case V4L2_PIX_FMT_MPEG4:
	case V4L2_PIX_FMT_H263:
	case V4L2_PIX_FMT_MPEG1:
	case V4L2_PIX_FMT_MPEG2:
		emi_bw = emi_bw * mp24_frm_scale[f_type] / (2 * STD_VDEC_FREQ);
		break;
	}

	/* transaction bytes to occupied BW */
	emi_bw = emi_bw * 4 / 3;

	/* bits/s to MBytes/s */
	emi_bw = emi_bw / (1024 * 1024) / 8;

	mtk_pm_qos_update_request(&vdec_qos_req_bw, (int)emi_bw);
#endif
}

static void mtk_vdec_emi_bw_end(void)
{
#if DEC_EMI_BW
	mtk_pm_qos_update_request(&vdec_qos_req_bw, 0);
#endif
}

void mtk_vdec_pmqos_prelock(struct mtk_vcodec_ctx *ctx, int hw_id)
{
#if DEC_DVFS
	mutex_lock(&ctx->dev->dec_dvfs_mutex);
	add_job(&ctx->id, &vdec_jobs);
	mutex_unlock(&ctx->dev->dec_dvfs_mutex);
#endif
}

void mtk_vdec_pmqos_begin_frame(struct mtk_vcodec_ctx *ctx, int hw_id)
{
	mtk_vdec_dvfs_begin(ctx);
	mtk_vdec_emi_bw_begin(ctx);
}

void mtk_vdec_pmqos_end_frame(struct mtk_vcodec_ctx *ctx, int hw_id)
{
	mtk_vdec_dvfs_end(ctx);
	mtk_vdec_emi_bw_end();
}


