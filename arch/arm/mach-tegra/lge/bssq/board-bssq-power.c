/*
 * Copyright (C) 2010-2011 NVIDIA, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */
#include <linux/i2c.h>
#include <linux/pda_power.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/max8907c.h>
#include <linux/regulator/max8907c-regulator.h>
// LGE_CHANGE [dojip.kim@lge.com] 2011-01-07, [LGE_AP20] DCDC for cpu 1.0V
#include <linux/regulator/max8952.h>
#include <linux/gpio.h>
#include <linux/io.h>

#include <mach/iomap.h>
#include <mach/irqs.h>

#include <mach-tegra/gpio-names.h>
#include <mach-tegra/fuse.h>
#include <mach-tegra/pm.h>
#include <mach-tegra/wakeups-t2.h>
#include <mach-tegra/board.h>

#define PMC_CTRL		0x0
#define PMC_CTRL_INTR_LOW	(1 << 17)

// LGE_CHANGE_S [yehan.ahn@lge.com] 2011-01-15, [LGE_AP20] general power control for device driver
int device_power_control(char* reg_id, bool on){
	 static struct regulator *device_regulator = NULL;
 
	 device_regulator = regulator_get(NULL, reg_id); 
	 if (!device_regulator) {
		 return -1;
	 }
	 if(on)
		 regulator_enable(device_regulator);
	 else
		 regulator_disable(device_regulator);
	 return 0;
}
 // LGE_CHANGE_E [yehan.ahn@lge.com] 2011-01-15, [LGE_AP20]

static struct regulator_consumer_supply max8907c_SD1_supply[] = {
	//REGULATOR_SUPPLY("vdd_cpu", NULL),
	REGULATOR_SUPPLY("vcc_io_1v2", NULL),
};

static struct regulator_consumer_supply max8907c_SD2_supply[] = {
	REGULATOR_SUPPLY("vdd_core", NULL),
	REGULATOR_SUPPLY("vdd_aon", NULL),
};

static struct regulator_consumer_supply max8907c_SD3_supply[] = {
	REGULATOR_SUPPLY("vddio_sys", NULL),
};

static struct regulator_consumer_supply max8907c_LDO1_supply[] = {
	REGULATOR_SUPPLY("vddio_rx_ddr", NULL),
};

static struct regulator_consumer_supply max8907c_LDO2_supply[] = {
	REGULATOR_SUPPLY("avdd_plla", NULL),
};

static struct regulator_consumer_supply max8907c_LDO3_supply[] = {
	//REGULATOR_SUPPLY("vdd_vcom_1v8b", NULL),
#if defined(CONFIG_KS1103)
	REGULATOR_SUPPLY("cam_vt_1v8", NULL),
#endif
};

static struct regulator_consumer_supply max8907c_LDO4_supply[] = {
	REGULATOR_SUPPLY("avdd_usb", NULL),
	REGULATOR_SUPPLY("avdd_usb_pll", NULL),
};

static struct regulator_consumer_supply max8907c_LDO5_supply[] = {
	REGULATOR_SUPPLY("vmmc", "sdhci-tegra.3"),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO6_supply[] = {
	REGULATOR_SUPPLY("avdd_hdmi_pll", NULL),
};

static struct regulator_consumer_supply max8907c_LDO7_supply[] = {
//	REGULATOR_SUPPLY("avddio_audio", NULL),
	REGULATOR_SUPPLY("vcc_sensor_3v0", NULL),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO8_supply[] = {
//	REGULATOR_SUPPLY("vdd_vcom_3v0", NULL),
	REGULATOR_SUPPLY("vcc_sensor_1v8", NULL),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO9_supply[] = {
//	REGULATOR_SUPPLY("vdd_cam1", NULL),
//	REGULATOR_SUPPLY("vcc_fuse_3v3", NULL),
};

static struct regulator_consumer_supply max8907c_LDO10_supply[] = {
//	REGULATOR_SUPPLY("avdd_usb_ic", NULL),
	REGULATOR_SUPPLY("avdd_hdmi", NULL),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO11_supply[] = {
//	REGULATOR_SUPPLY("vddio_pex_clk", NULL),
//	REGULATOR_SUPPLY("avdd_hdmi", NULL),
	REGULATOR_SUPPLY("vcc_prox_3v0", NULL),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO12_supply[] = {
//	REGULATOR_SUPPLY("vddio_sdio", NULL),		//TEGRA
	REGULATOR_SUPPLY("vmmc", "sdhci-tegra.2"),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO13_supply[] = {
//	REGULATOR_SUPPLY("vdd_vcore_phtn", NULL),
//	REGULATOR_SUPPLY("vdd_vcore_af", NULL),
	REGULATOR_SUPPLY("vcc_motor_3v0", NULL),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO14_supply[] = {
//	REGULATOR_SUPPLY("avdd_vdac", NULL),
#if defined(CONFIG_KS1001) || defined(CONFIG_KS1103)
	REGULATOR_SUPPLY("cam_avdd_2v8", NULL),
#endif
};

static struct regulator_consumer_supply max8907c_LDO15_supply[] = {
//	REGULATOR_SUPPLY("vdd_vcore_temp", NULL),
//	REGULATOR_SUPPLY("vdd_vcore_hdcp", NULL),
	REGULATOR_SUPPLY("vcc_aptemp_3v3", NULL),	//LGE
};

static struct regulator_consumer_supply max8907c_LDO16_supply[] = {
//	REGULATOR_SUPPLY("vdd_vbrtr", NULL),
#if defined(CONFIG_KS1103)
	REGULATOR_SUPPLY("cam_lvdd_2v8", NULL),
#endif
};

static struct regulator_consumer_supply max8907c_LDO17_supply[] = {
	REGULATOR_SUPPLY("vddio_mipi", NULL),
};

static struct regulator_consumer_supply max8907c_LDO18_supply[] = {
	REGULATOR_SUPPLY("vddio_vi", NULL),
	REGULATOR_SUPPLY("vcsi", "tegra_camera"),
};

static struct regulator_consumer_supply max8907c_LDO19_supply[] = {
//	REGULATOR_SUPPLY("vddio_lx", NULL),
#if defined(CONFIG_KS1103)
		REGULATOR_SUPPLY("cam_vt_2v8", NULL),
#endif
};

static struct regulator_consumer_supply max8907c_LDO20_supply[] = {
//	REGULATOR_SUPPLY("vddio_ddr_1v2", NULL),
//	REGULATOR_SUPPLY("vddio_hsic", NULL),
#if defined(CONFIG_KS1001) || defined(CONFIG_KS1103)
	REGULATOR_SUPPLY("cam_iovdd_1v8", NULL),
#endif
};

static struct regulator_consumer_supply max8907c_WLED_supply[] = {
// LGE_CHANGES [sungyel.bae@lge.com] 2011-01-21, [LGE_AP20] TOUCH_LED
	REGULATOR_SUPPLY("vcc_wled", NULL),
// LGE_CHANGEE [sungyel.bae@lge.com] 2011-01-21, [LGE_AP20] TOUCH_LED
};

//LGE_CHANGE_S [heejeong.seo@lge.com] 2011-01-14, [LGE_AP20] audio enable
static struct regulator_consumer_supply max8907c_DUMMY_supply[] = {
		REGULATOR_SUPPLY("DBVDD", NULL),
		REGULATOR_SUPPLY("DCVDD", NULL),
		REGULATOR_SUPPLY("AVDD1", NULL),
		REGULATOR_SUPPLY("AVDD2", NULL),
		REGULATOR_SUPPLY("CPVDD", NULL),
		REGULATOR_SUPPLY("SPKVDD1", NULL),
		REGULATOR_SUPPLY("SPKVDD2", NULL),
};

#define MAX8907C_REGULATOR_DEVICE(_id, _minmv, _maxmv)			\
static struct regulator_init_data max8907c_##_id##_data = {		\
	.constraints = {						\
		.min_uV = (_minmv),					\
		.max_uV = (_maxmv),					\
		.valid_modes_mask = (REGULATOR_MODE_NORMAL |		\
				     REGULATOR_MODE_STANDBY),		\
		.valid_ops_mask = (REGULATOR_CHANGE_MODE |		\
				   REGULATOR_CHANGE_STATUS |		\
				   REGULATOR_CHANGE_VOLTAGE |		\
				   REGULATOR_CHANGE_CURRENT	 ),		\
	},								\
	.num_consumer_supplies = ARRAY_SIZE(max8907c_##_id##_supply),	\
	.consumer_supplies = max8907c_##_id##_supply,			\
};									\
static struct platform_device max8907c_##_id##_device = {		\
	.name	= "max8907c-regulator",					\
	.id	= MAX8907C_##_id,					\
	.dev	= {							\
		.platform_data = &max8907c_##_id##_data,		\
	},								\
}

//MAX8907C_REGULATOR_DEVICE(SD1, 637500, 1425000);
MAX8907C_REGULATOR_DEVICE(SD1, 1200000, 1200000);
MAX8907C_REGULATOR_DEVICE(SD2, 637500, 1425000);
MAX8907C_REGULATOR_DEVICE(SD3, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO1, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO2, 650000, 2225000);
MAX8907C_REGULATOR_DEVICE(LDO3, 750000, 3900000); //20110525 calvin.hwang@lge.com Camsensor ks1001 merge
MAX8907C_REGULATOR_DEVICE(LDO4, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO5, 750000, 3900000);
#if defined (CONFIG_LU8800) || defined (CONFIG_KU8800) || defined (CONFIG_SU880)
MAX8907C_REGULATOR_DEVICE(LDO6, 1800000, 1800000);
#else
MAX8907C_REGULATOR_DEVICE(LDO6, 750000, 3900000);
#endif

//MAX8907C_REGULATOR_DEVICE(LDO7, 750000, 3900000);
//MAX8907C_REGULATOR_DEVICE(LDO8, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO7, 3000000, 3000000);
MAX8907C_REGULATOR_DEVICE(LDO8, 1800000, 1800000);

MAX8907C_REGULATOR_DEVICE(LDO9, 750000, 3900000);

#if defined (CONFIG_LU8800) || defined (CONFIG_KU8800) || defined (CONFIG_SU880) || defined (CONFIG_KS1103)
MAX8907C_REGULATOR_DEVICE(LDO10, 3300000, 3300000);
#else
MAX8907C_REGULATOR_DEVICE(LDO10, 750000, 3900000);//20110525 calvin.hwang@lge.com Camsensor ks1001 merge
#endif

MAX8907C_REGULATOR_DEVICE(LDO11, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO12, 750000, 3900000);

//MAX8907C_REGULATOR_DEVICE(LDO13, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO13, 3000000, 3000000);

//MAX8907C_REGULATOR_DEVICE(LDO14, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO14, 2800000, 2800000);
// 20110524 bg80.song@lge.com AP Temp Sensor Bring-up [S]
//MAX8907C_REGULATOR_DEVICE(LDO15, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO15, 3300000, 3300000);
// 20110524 bg80.song@lge.com AP Temp Sensor Bring-up [E]

MAX8907C_REGULATOR_DEVICE(LDO16, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO17, 650000, 2225000);
MAX8907C_REGULATOR_DEVICE(LDO18, 650000, 2225000);
MAX8907C_REGULATOR_DEVICE(LDO19, 750000, 3900000);
MAX8907C_REGULATOR_DEVICE(LDO20, 750000, 3900000);
//[heejeong.seo@lge.com] 2011-01-14, [LGE_AP20] audio enable
#ifndef CONFIG_MACH_BSSQ
MAX8907C_REGULATOR_DEVICE(DUMMY, 1250000, 5000000); //wm8994
#endif
MAX8907C_REGULATOR_DEVICE(WLED, 3000000, 3000000);	//	cbi
//20110502 hyeongwon.oh@lge.com add battery driver [S]
struct platform_device star_battery_device = {
   .name = "bssq_battery",
   .id   = -1,
};
//20110502 hyeongwon.oh@lge.com add battery driver [E]

static struct platform_device *bssq_max8907c_power_devices[] = {
	&max8907c_SD1_device,
	&max8907c_SD2_device,
	&max8907c_SD3_device,
	&max8907c_LDO1_device,
	&max8907c_LDO2_device,
	&max8907c_LDO3_device,
	&max8907c_LDO4_device,
	&max8907c_LDO5_device,
	&max8907c_LDO6_device,
	&max8907c_LDO7_device,
	&max8907c_LDO8_device,
	&max8907c_LDO9_device,
	&max8907c_LDO10_device,
	&max8907c_LDO11_device,
	&max8907c_LDO12_device,
	&max8907c_LDO13_device,
	&max8907c_LDO14_device,
	&max8907c_LDO15_device,
	&max8907c_LDO16_device,
	&max8907c_LDO17_device,
	&max8907c_LDO18_device,
	&max8907c_LDO19_device,
	&max8907c_LDO20_device,
#ifndef CONFIG_MACH_BSSQ
	&max8907c_DUMMY_device,	//[heejeong.seo@lge.com] 2011-01-14, [LGE_AP20] audio enable
#endif
	&max8907c_WLED_device,	//[sungyel.bae@lge.com] 2011-01-22, [LGE_AP20] touch_led
	&star_battery_device,	//20110502 hyeongwon.oh@lge.com add battery driver 
};

static int bssq_max8907c_setup(void)
{
	int ret;

	/*
	 * Configure PWREN, and attach CPU V1 rail to it.
	 * TODO: h/w events (power cycle, reset, battery low) auto-disables PWREN.
	 * Only soft reset (not supported) requires s/w to disable PWREN explicitly
	 */
	ret = max8907c_pwr_en_config();
	if (ret != 0)
		return ret;

	return max8907c_pwr_en_attach();
}

static struct max8907c_platform_data max8907c_pdata = {
	.num_subdevs = ARRAY_SIZE(bssq_max8907c_power_devices),
	.subdevs = bssq_max8907c_power_devices,
	.irq_base = TEGRA_NR_IRQS,
//	.max8907c_setup = bssq_max8907c_setup,	// if cpu and core power seperate, this is not needed.
};

static struct regulator_consumer_supply max8952_MODE0_supply[] = {
};

static struct regulator_consumer_supply max8952_MODE1_supply[] = {
};

static struct regulator_consumer_supply max8952_MODE2_supply[] = {
};

static struct regulator_consumer_supply max8952_MODE3_supply[] = {
	REGULATOR_SUPPLY("vdd_cpu", NULL),	
};

#define MAX8952_REGULATOR_INIT(_id, _minmv, _maxmv)			\
static struct regulator_init_data max8952_##_id##_data = {		\
	.constraints = {						\
		.min_uV = (_minmv),					\
		.max_uV = (_maxmv),					\
		.valid_modes_mask = (REGULATOR_MODE_NORMAL |		\
				     REGULATOR_MODE_STANDBY),		\
		.valid_ops_mask = (REGULATOR_CHANGE_MODE |		\
				   REGULATOR_CHANGE_STATUS |		\
				   REGULATOR_CHANGE_VOLTAGE),		\
	},								\
	.num_consumer_supplies = ARRAY_SIZE(max8952_##_id##_supply),	\
	.consumer_supplies = max8952_##_id##_supply,			\
};									\
static struct platform_device max8952_##_id##_device = {		\
	.id	= MAX8952_##_id,					\
	.dev	= {							\
		.platform_data = &max8952_##_id##_data,			\
	},								\
}

MAX8952_REGULATOR_INIT(MODE0, 770000, 1400000);
MAX8952_REGULATOR_INIT(MODE1, 770000, 1400000);
MAX8952_REGULATOR_INIT(MODE2, 770000, 1400000);
MAX8952_REGULATOR_INIT(MODE3, 770000, 1400000);

static struct platform_device *bssq_max8952_power_devices[] = {
	&max8952_MODE0_device,
	&max8952_MODE1_device,
	&max8952_MODE2_device,
	&max8952_MODE3_device,
};

static struct max8952_platform_data max8952_pdata = {
	.num_subdevs = ARRAY_SIZE(bssq_max8952_power_devices),
	.subdevs = bssq_max8952_power_devices,
};
// LGE_CHANGE_E [dojip.kim@lge.com] 2011-01-07, [LGE_AP20] DCDC for cpu 1.0V

static struct i2c_board_info __initdata bssq_regulators[] = {
	{
		I2C_BOARD_INFO("max8907c", 0x3C),
		.irq = INT_EXTERNAL_PMU,
		.platform_data	= &max8907c_pdata,
	},
	// LGE_CHANGE [dojip.kim@lge.com] 2011-01-07, [LGE_AP20] DCDC for cpu 1.0V
	{
		I2C_BOARD_INFO("max8952", 0x60),
		.platform_data = &max8952_pdata,
	},
};

static void bssq_board_suspend(int lp_state, enum suspend_stage stg)
{
	if ((lp_state == TEGRA_SUSPEND_LP1) && (stg == TEGRA_SUSPEND_BEFORE_CPU))
		tegra_console_uart_suspend();
}

static void bssq_board_resume(int lp_state, enum resume_stage stg)
{
	if ((lp_state == TEGRA_SUSPEND_LP1) && (stg == TEGRA_RESUME_AFTER_CPU))
		tegra_console_uart_resume();
}

static struct tegra_suspend_platform_data bssq_suspend_data = {
	.cpu_timer	= 2000,
	.cpu_off_timer	= 1000,
	.suspend_mode	= TEGRA_SUSPEND_LP0,
#ifdef CONFIG_MACH_BSSQ
	.core_timer	= 0x7e7e,	//BSSQ model use ventana value
	.core_off_timer = 0xf,	//BSSQ model use ventana value
#else
//	.core_timer	= 0x7e,
//	.core_off_timer = 0xc00,
#endif
	.corereq_high	= true,
	.sysclkreq_high	= true,
	.combined_req   = false,	// cpu and core power seperate. set false. (in K36 .separate_req = true)
	.board_suspend = bssq_board_suspend,
	.board_resume = bssq_board_resume,
};

int __init bssq_regulator_init(void)
{
	void __iomem *pmc = IO_ADDRESS(TEGRA_PMC_BASE);
	void __iomem *chip_id = IO_ADDRESS(TEGRA_APB_MISC_BASE) + 0x804;
	u32 pmc_ctrl;
	u32 minor;

	minor = (readl(chip_id) >> 16) & 0xf;
	/* A03 (but not A03p) chips do not support LP0 */
	if (minor == 3 && !(tegra_spare_fuse(18) || tegra_spare_fuse(19)))
		bssq_suspend_data.suspend_mode = TEGRA_SUSPEND_LP1;

	/* configure the power management controller to trigger PMU
	 * interrupts when low */
	pmc_ctrl = readl(pmc + PMC_CTRL);
	writel(pmc_ctrl | PMC_CTRL_INTR_LOW, pmc + PMC_CTRL);

	i2c_register_board_info(4, bssq_regulators, ARRAY_SIZE(bssq_regulators));

	tegra_deep_sleep = max8907c_deep_sleep;

	tegra_init_suspend(&bssq_suspend_data);

	return 0;
}
