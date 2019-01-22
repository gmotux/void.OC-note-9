/* drivers/gpu/t6xx/kbase/src/platform/gpu_exynos8890.c
 *
 * Copyright 2011 by S.LSI. Samsung Electronics Inc.
 * San#24, Nongseo-Dong, Giheung-Gu, Yongin, Korea
 *
 * Samsung SoC Mali-T604 DVFS driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software FoundatIon.
 */

/**
 * @file gpu_exynos8890.c
 * DVFS
 */

#include <mali_kbase.h>

#include <linux/regulator/driver.h>
#include <linux/pm_qos.h>
#include <linux/delay.h>
#include <linux/smc.h>
#include <linux/of.h>

#ifdef CONFIG_EXYNOS_ASV
#include <soc/samsung/asv-exynos.h>
#endif

#ifdef CONFIG_CAL_IF
#include <soc/samsung/cal-if.h>
#endif

#ifdef CONFIG_EXYNOS_PD
#include <soc/samsung/exynos-pd.h>
#endif
#ifdef CONFIG_EXYNOS_PMU
#include <soc/samsung/exynos-pmu.h>
#endif

#include <linux/clk.h>


#include "mali_kbase_platform.h"
#include "gpu_dvfs_handler.h"
#include "gpu_dvfs_governor.h"
#include "gpu_control.h"
#include "../mali_midg_regmap.h"


extern struct kbase_device *pkbdev;
#define EXYNOS_PMU_G3D_STATUS		0x4044
#define LOCAL_PWR_CFG				(0xF << 0)

#ifdef CONFIG_MALI_DVFS
#define CPU_MAX INT_MAX
#else
#define CPU_MAX -1
#endif

#ifndef KHZ
#define KHZ (1000)
#endif

#ifdef CONFIG_EXYNOS_BUSMONITOR
void __iomem *g3d0_outstanding_regs;
void __iomem *g3d1_outstanding_regs;
#endif /* CONFIG_EXYNOS_BUSMONITOR */

/*  clk,vol,abb,min,max,down stay, pm_qos mem, pm_qos int, pm_qos cpu_kfc_min, pm_qos cpu_egl_max */
static gpu_dvfs_info gpu_dvfs_table_default[] = {
	{676, 850000, 0, 88, 100, 5, 0, 2002000, 400000, 1794000, CPU_MAX},
	{637, 800000, 0, 78,  99, 5, 0, 2002000, 400000, 1794000, CPU_MAX},
	{598, 800000, 0, 78,  99, 5, 0, 2002000, 400000, 1794000, CPU_MAX},
	{572, 800000, 0, 78,  85, 5, 0, 1794000, 400000, 1794000, CPU_MAX},
	{546, 800000, 0, 78,  85, 9, 0, 1540000, 400000, 1456000, CPU_MAX},
	{455, 800000, 0, 78,  85, 1, 0, 1352000, 400000, 1248000, CPU_MAX},
	{338, 800000, 0, 78,  85, 1, 0, 1014000, 267000,  949000, CPU_MAX},
	{260, 800000, 0, 78,  85, 1, 0,  421000, 178000,       0, CPU_MAX},
};

static int mif_min_table[] = {
	 100000,  133000,  167000,
	 276000,  348000,  416000,
	 543000,  632000,  828000,
	1026000, 1264000, 1456000,
	1552000,
};

static gpu_attribute gpu_config_attributes[] = {
	{GPU_MAX_CLOCK, 676},
	{GPU_MAX_CLOCK_LIMIT, 676},
	{GPU_MIN_CLOCK, 260},
	{GPU_DVFS_START_CLOCK, 260},
	{GPU_DVFS_BL_CONFIG_CLOCK, 260},
	{GPU_GOVERNOR_TYPE, G3D_DVFS_GOVERNOR_INTERACTIVE},
	{GPU_GOVERNOR_START_CLOCK_DEFAULT, 260},
	{GPU_GOVERNOR_START_CLOCK_INTERACTIVE, 260},
	{GPU_GOVERNOR_START_CLOCK_STATIC, 260},
	{GPU_GOVERNOR_START_CLOCK_BOOSTER, 260},
	{GPU_GOVERNOR_TABLE_DEFAULT, (uintptr_t)&gpu_dvfs_table_default},
	{GPU_GOVERNOR_TABLE_INTERACTIVE, (uintptr_t)&gpu_dvfs_table_default},
	{GPU_GOVERNOR_TABLE_STATIC, (uintptr_t)&gpu_dvfs_table_default},
	{GPU_GOVERNOR_TABLE_BOOSTER, (uintptr_t)&gpu_dvfs_table_default},
	{GPU_GOVERNOR_TABLE_SIZE_DEFAULT, GPU_DVFS_TABLE_LIST_SIZE(gpu_dvfs_table_default)},
	{GPU_GOVERNOR_TABLE_SIZE_INTERACTIVE, GPU_DVFS_TABLE_LIST_SIZE(gpu_dvfs_table_default)},
	{GPU_GOVERNOR_TABLE_SIZE_STATIC, GPU_DVFS_TABLE_LIST_SIZE(gpu_dvfs_table_default)},
	{GPU_GOVERNOR_TABLE_SIZE_BOOSTER, GPU_DVFS_TABLE_LIST_SIZE(gpu_dvfs_table_default)},
	{GPU_GOVERNOR_INTERACTIVE_HIGHSPEED_CLOCK, 676},
	{GPU_GOVERNOR_INTERACTIVE_HIGHSPEED_LOAD, 95},
	{GPU_GOVERNOR_INTERACTIVE_HIGHSPEED_DELAY, 0},
	{GPU_DEFAULT_VOLTAGE, 800000},
	{GPU_COLD_MINIMUM_VOL, 0},
	{GPU_VOLTAGE_OFFSET_MARGIN, 37500},
	{GPU_TMU_CONTROL, 1},
	{GPU_TEMP_THROTTLING1, 676},
	{GPU_TEMP_THROTTLING2, 637},
	{GPU_TEMP_THROTTLING3, 598},
	{GPU_TEMP_THROTTLING4, 455},
	{GPU_TEMP_THROTTLING5, 572},
	{GPU_TEMP_TRIPPING, 260},
	{GPU_POWER_COEFF, 625}, /* all core on param */
	{GPU_DVFS_TIME_INTERVAL, 5},
	{GPU_DEFAULT_WAKEUP_LOCK, 1},
	{GPU_BUS_DEVFREQ, 0},
	{GPU_DYNAMIC_ABB, 0},
	{GPU_EARLY_CLK_GATING, 0},
	{GPU_DVS, 0},
	{GPU_INTER_FRAME_PM, 1},
	{GPU_PERF_GATHERING, 0},
	{GPU_RUNTIME_PM_DELAY_TIME, 50},
	{GPU_DVFS_POLLING_TIME, 30},
	{GPU_PMQOS_INT_DISABLE, 1},
	{GPU_PMQOS_MIF_MAX_CLOCK, 2002000},
	{GPU_PMQOS_MIF_MAX_CLOCK_BASE, 676},
	{GPU_CL_DVFS_START_BASE, 455},
	{GPU_DEBUG_LEVEL, DVFS_WARNING},
	{GPU_TRACE_LEVEL, TRACE_ALL},
	{GPU_MO_MIN_CLOCK, 546},
	{GPU_BOOST_EGL_MIN_LOCK, 1872000},
#ifdef CONFIG_MALI_VK_BOOST
	{GPU_VK_BOOST_MAX_LOCK, 338},
	{GPU_VK_BOOST_MIF_MIN_LOCK, 1794000},
#endif
	{GPU_CONFIG_LIST_END, 0}
};

int gpu_dvfs_decide_max_clock(struct exynos_context *platform)
{
	if (!platform)
		return -1;

	return 0;
}

void *gpu_get_config_attributes(void)
{
	return &gpu_config_attributes;
}

uintptr_t gpu_get_max_freq(void)
{
	return gpu_get_attrib_data(gpu_config_attributes, GPU_MAX_CLOCK) * 1000;
}

uintptr_t gpu_get_min_freq(void)
{
	return gpu_get_attrib_data(gpu_config_attributes, GPU_MIN_CLOCK) * 1000;
}

struct clk *vclk_g3d;
#ifdef CONFIG_REGULATOR
struct regulator *g3d_regulator;
struct regulator *g3d_m_regulator;
#endif /* CONFIG_REGULATOR */

int gpu_is_power_on(void)
{
	unsigned int val = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
	val = __raw_readl(EXYNOS_PMU_G3D_STATUS);
#else
#ifdef CONFIG_EXYNOS_PMU
	exynos_pmu_read(EXYNOS_PMU_G3D_STATUS, &val);
#else
	val = 0xf;
#endif
#endif

	return ((val & LOCAL_PWR_CFG) == LOCAL_PWR_CFG) ? 1 : 0;
}

int gpu_power_init(struct kbase_device *kbdev)
{
	struct exynos_context *platform = (struct exynos_context *) kbdev->platform_context;

	if (!platform)
		return -ENODEV;

	GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "power initialized\n");

	return 0;
}

int gpu_get_cur_clock(struct exynos_context *platform)
{
	if (!platform)
		return -ENODEV;
	return cal_dfs_get_rate(platform->g3d_cmu_cal_id)/KHZ;
}


int gpu_register_dump(void)
{
	return 0;
}

#ifdef CONFIG_MALI_DVFS
static int gpu_set_dvfs(struct exynos_context *platform, int clk)
{
	unsigned long g3d_rate = clk * KHZ;
	int ret = 0;

#ifdef CONFIG_MALI_RT_PM
	if (platform->exynos_pm_domain)
		mutex_lock(&platform->exynos_pm_domain->access_lock);

	if (!gpu_is_power_on()) {
		ret = -1;
		GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "%s: can't set clock in the power-off state!\n", __func__);
		goto err;
	}
#endif /* CONFIG_MALI_RT_PM */

	if (clk == platform->cur_clock) {
		ret = 0;
		GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "%s: skipped to set clock for %dMhz!\n", __func__, platform->cur_clock);

#ifdef CONFIG_MALI_RT_PM
		if (platform->exynos_pm_domain)
			mutex_unlock(&platform->exynos_pm_domain->access_lock);
#endif
		return ret;
	}

	cal_dfs_set_rate(platform->g3d_cmu_cal_id ,g3d_rate);

	platform->cur_clock = cal_dfs_get_rate(platform->g3d_cmu_cal_id)/KHZ;

	GPU_LOG(DVFS_INFO, LSI_CLOCK_VALUE, g3d_rate/KHZ, platform->cur_clock,
		"[id: %x] clock set: %ld, clock get: %d\n", platform->g3d_cmu_cal_id, g3d_rate/KHZ, platform->cur_clock);

#ifdef CONFIG_MALI_RT_PM
err:
	if (platform->exynos_pm_domain)
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
#endif /* CONFIG_MALI_RT_PM */
	return ret;
}

static int gpu_get_clock(struct kbase_device *kbdev)
{
#ifdef CONFIG_OF
	struct device_node *np = NULL;
#endif
	struct exynos_context *platform = (struct exynos_context *) kbdev->platform_context;
	if (!platform)
		return -ENODEV;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

#ifdef CONFIG_OF
#ifdef CONFIG_CAL_IF
	np = kbdev->dev->of_node;

	if (np != NULL) {
		if (of_property_read_u32(np, "g3d_cmu_cal_id", &platform->g3d_cmu_cal_id)) {
			GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: failed to get CMU CAL ID [ACPM_DVFS_G3D]\n", __func__);
			return -1;
		}
	}
#endif
#endif

	return 0;
}
#endif

int gpu_clock_init(struct kbase_device *kbdev)
{
#ifdef CONFIG_MALI_DVFS
	int ret;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	ret = gpu_get_clock(kbdev);
	if (ret < 0)
		return -1;

#ifdef CONFIG_EXYNOS_BUSMONITOR
	g3d0_outstanding_regs = ioremap(0x14A00000, SZ_1K);
	g3d1_outstanding_regs = ioremap(0x14A20000, SZ_1K);
#endif /* CONFIG_EXYNOS_BUSMONITOR */

	GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "clock initialized\n");
#endif
	return 0;
}

int gpu_get_cur_voltage(struct exynos_context *platform)
{
	int ret = 0;
#ifdef CONFIG_REGULATOR
	if (!g3d_regulator) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: regulator is not initialized\n", __func__);
		return -1;
	}

	ret = regulator_get_voltage(g3d_regulator);
#endif /* CONFIG_REGULATOR */
	return ret;
}

static struct gpu_control_ops ctr_ops = {
	.is_power_on = gpu_is_power_on,
#ifdef CONFIG_MALI_DVFS
	.set_dvfs = gpu_set_dvfs,
	.set_voltage = NULL,
	.set_voltage_pre = NULL,
	.set_voltage_post = NULL,
	.set_clock_to_osc = NULL,
	.set_clock = NULL,
	.set_clock_pre = NULL,
	.set_clock_post = NULL,
	.enable_clock = NULL,
	.disable_clock = NULL,
#endif
};

struct gpu_control_ops *gpu_get_control_ops(void)
{
	return &ctr_ops;
}

#ifdef CONFIG_REGULATOR
extern int s2m_set_dvs_pin(bool gpio_val);
int gpu_enable_dvs(struct exynos_context *platform)
{
#ifdef CONFIG_MALI_RT_PM
	if (!platform->dvs_status)
		return 0;

	if (!gpu_is_power_on()) {
		GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "%s: can't set dvs in the power-off state!\n", __func__);
		return -1;
	}

#if defined(CONFIG_REGULATOR_S2MPS16)
	/* Do not need to enable dvs during suspending */
	if (!pkbdev->pm.suspending) {
		if (cal_dfs_ext_ctrl(dvfs_g3d, cal_dfs_dvs, 1) != 0) {
			GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: failed to enable dvs\n", __func__);
			return -1;
		}
	}
#endif /* CONFIG_REGULATOR_S2MPS16 */

	GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "dvs is enabled (vol: %d)\n", gpu_get_cur_voltage(platform));
#endif
	return 0;
}

int gpu_disable_dvs(struct exynos_context *platform)
{
	if (!platform->dvs_status)
		return 0;

#ifdef CONFIG_MALI_RT_PM
#if defined(CONFIG_REGULATOR_S2MPS16)
	if (cal_dfs_ext_ctrl(dvfs_g3d, cal_dfs_dvs, 0) != 0) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: failed to disable dvs\n", __func__);
		return -1;
	}
#endif /* CONFIG_REGULATOR_S2MPS16 */

	GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "dvs is disabled (vol: %d)\n", gpu_get_cur_voltage(platform));
#endif
	return 0;
}

int gpu_inter_frame_power_on(struct exynos_context *platform)
{
#ifdef CONFIG_MALI_RT_PM
	int status;

	if (!platform->inter_frame_pm_status)
		return 0;

	mutex_lock(&platform->exynos_pm_domain->access_lock);

	status = cal_pd_status(platform->exynos_pm_domain->cal_pdid);
	if (status) {
		GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "%s: status checking : Already gpu inter frame power on\n", __func__);
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
		return 0;
	}

	if (cal_pd_control(platform->exynos_pm_domain->cal_pdid, 1) != 0) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: failed to gpu inter frame power on\n", __func__);
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
		return -1;
	}

	status = cal_pd_status(platform->exynos_pm_domain->cal_pdid);
	if (!status) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: status error : gpu inter frame power on\n", __func__);
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
		return -1;
	}

	mutex_unlock(&platform->exynos_pm_domain->access_lock);
	GPU_LOG(DVFS_INFO, LSI_IFPM_POWER_ON, 0u, 0u, "gpu inter frame power on\n");
#endif
	return 0;
}

int gpu_inter_frame_power_off(struct exynos_context *platform)
{
#ifdef CONFIG_MALI_RT_PM
	int status;

	if (!platform->inter_frame_pm_status)
		return 0;

	mutex_lock(&platform->exynos_pm_domain->access_lock);

	status = cal_pd_status(platform->exynos_pm_domain->cal_pdid);
	if (!status) {
		GPU_LOG(DVFS_INFO, DUMMY, 0u, 0u, "%s: status checking: Already gpu inter frame power off\n", __func__);
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
		return 0;
	}

	if (cal_pd_control(platform->exynos_pm_domain->cal_pdid, 0) != 0) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: failed to gpu inter frame power off\n", __func__);
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
		return -1;
	}

	status = cal_pd_status(platform->exynos_pm_domain->cal_pdid);
	if (status) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: status error :  gpu inter frame power off\n", __func__);
		mutex_unlock(&platform->exynos_pm_domain->access_lock);
		return -1;
	}

	mutex_unlock(&platform->exynos_pm_domain->access_lock);
	GPU_LOG(DVFS_INFO, LSI_IFPM_POWER_OFF, 0u, 0u, "gpu inter frame power off\n");
#endif
	return 0;
}

#ifdef CONFIG_MALI_ASV_CALIBRATION_SUPPORT
struct workqueue_struct *gpu_asv_cali_wq = NULL;
struct delayed_work gpu_asv_cali_stop_work;

static void gpu_asv_calibration_stop_callback(struct work_struct *data)
{
	struct exynos_context *platform = (struct exynos_context *) pkbdev->platform_context;

	if (!platform) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: platform context is null\n", __func__);
		return;
	}

	gpu_dvfs_clock_lock(GPU_DVFS_MAX_UNLOCK, ASV_CALI_LOCK, 0);
	gpu_dvfs_clock_lock(GPU_DVFS_MIN_UNLOCK, ASV_CALI_LOCK, 0);
	gpu_control_power_policy_set(pkbdev, "demand");
	platform->gpu_auto_cali_status = false;
}

int gpu_asv_calibration_start(void)
{
	struct exynos_context *platform = (struct exynos_context *) pkbdev->platform_context;

	if (!platform) {
		GPU_LOG(DVFS_ERROR, DUMMY, 0u, 0u, "%s: platform context is null\n", __func__);
		return -ENODEV;
	}

	platform->gpu_auto_cali_status = true;
	gpu_control_power_policy_set(pkbdev, "always_on");
	gpu_dvfs_clock_lock(GPU_DVFS_MAX_LOCK, ASV_CALI_LOCK, 676);
	gpu_dvfs_clock_lock(GPU_DVFS_MIN_LOCK, ASV_CALI_LOCK, 676);

	if (NULL == gpu_asv_cali_wq) {
		INIT_DELAYED_WORK(&gpu_asv_cali_stop_work, gpu_asv_calibration_stop_callback);
		gpu_asv_cali_wq = create_workqueue("g3d_asv_cali");

		queue_delayed_work_on(0, gpu_asv_cali_wq,
				&gpu_asv_cali_stop_work, msecs_to_jiffies(15000));	/* 15 second */
}

	return 0;
}
#endif

int gpu_regulator_init(struct exynos_context *platform)
{
	return 0;
}
#endif /* CONFIG_REGULATOR */

int *get_mif_table(int *size)
{
	*size = ARRAY_SIZE(mif_min_table);
	return mif_min_table;
}
