#ifndef MY_ABC_HERE
#define MY_ABC_HERE
#endif
/*
 * Copyright (C) 2010 Marvell International Ltd.
 *		Zhangfei Gao <zhangfei.gao@marvell.com>
 *		Kevin Wang <dwang4@marvell.com>
 *		Mingwei Wang <mwwang@marvell.com>
 *		Philip Rakity <prakity@marvell.com>
 *		Mark Brown <markb@marvell.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/err.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/platform_data/pxa_sdhci.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/mbus.h>

#include "sdhci.h"
#include "sdhci-pltfm.h"

#define PXAV3_RPM_DELAY_MS     50

#define SD_CLOCK_BURST_SIZE_SETUP		0x10A
#define SDCLK_SEL	0x100
#define SDCLK_DELAY_SHIFT	9
#define SDCLK_DELAY_MASK	0x1f

#if defined(MY_DEF_HERE)
#define SD_EXTRA_PARAM_REG	0x100
#else /* MY_DEF_HERE */
#define SD_CFG_FIFO_PARAM       0x100
#endif /* MY_DEF_HERE */
#define SDCFG_GEN_PAD_CLK_ON	(1<<6)
#define SDCFG_GEN_PAD_CLK_CNT_MASK	0xFF
#define SDCFG_GEN_PAD_CLK_CNT_SHIFT	24
#if defined(MY_DEF_HERE)
#define SD_FIFO_PARAM_REG	0x104
#define SD_USE_DAT3		BIT(7)
#define SD_OVRRD_CLK_OEN	BIT(11)
#define SD_FORCE_CLK_ON		BIT(12)
#endif /* MY_DEF_HERE */

#define SD_SPI_MODE          0x108
#define SD_CE_ATA_1          0x10C
#if defined(MY_DEF_HERE)
#define SDCE_MMC_CARD		BIT(28)
#endif /* MY_DEF_HERE */

#define SD_CE_ATA_2          0x10E
#define SDCE_MISC_INT		(1<<2)
#define SDCE_MISC_INT_EN	(1<<1)

struct sdhci_pxa {
	struct clk *clk_core;
	struct clk *clk_io;
	u8	power_mode;
	void __iomem *sdio3_conf_reg;
#if defined(MY_DEF_HERE)
	void __iomem *mbus_win_regs;
	bool dat3_cd_enabled;
#endif /* MY_DEF_HERE */
};

/*
 * These registers are relative to the second register region, for the
 * MBus bridge.
 */
#define SDHCI_WINDOW_CTRL(i)	(0x80 + ((i) << 3))
#define SDHCI_WINDOW_BASE(i)	(0x84 + ((i) << 3))
#define SDHCI_MAX_WIN_NUM	8

/*
 * Fields below belong to SDIO3 Configuration Register (third register
 * region for the Armada 38x flavor)
 */

#define SDIO3_CONF_CLK_INV	BIT(0)
#define SDIO3_CONF_SD_FB_CLK	BIT(2)

#if defined(MY_DEF_HERE)
static int mv_conf_mbus_windows(struct device *dev, void __iomem *regs,
				const struct mbus_dram_target_info *dram)
{
	int i;

	if (!dram) {
		dev_err(dev, "no mbus dram info\n");
		return -EINVAL;
	}
#else /* MY_DEF_HERE */
static int mv_conf_mbus_windows(struct platform_device *pdev,
				const struct mbus_dram_target_info *dram)
{
	int i;
	void __iomem *regs;
	struct resource *res;

	if (!dram) {
		dev_err(&pdev->dev, "no mbus dram info\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(&pdev->dev, "cannot get mbus registers\n");
		return -EINVAL;
	}

	regs = ioremap(res->start, resource_size(res));
	if (!regs) {
		dev_err(&pdev->dev, "cannot map mbus registers\n");
		return -ENOMEM;
	}
#endif /* MY_DEF_HERE */

	for (i = 0; i < SDHCI_MAX_WIN_NUM; i++) {
		writel(0, regs + SDHCI_WINDOW_CTRL(i));
		writel(0, regs + SDHCI_WINDOW_BASE(i));
	}

	for (i = 0; i < dram->num_cs; i++) {
		const struct mbus_dram_window *cs = dram->cs + i;

		/* Write size, attributes and target id to control register */
		writel(((cs->size - 1) & 0xffff0000) |
			(cs->mbus_attr << 8) |
			(dram->mbus_dram_target_id << 4) | 1,
			regs + SDHCI_WINDOW_CTRL(i));
		/* Write base address to base register */
		writel(cs->base, regs + SDHCI_WINDOW_BASE(i));
	}

#if defined(MY_DEF_HERE)
//do nothing
#else /* MY_DEF_HERE */
	iounmap(regs);
#endif /* MY_DEF_HERE */

	return 0;
}

static int armada_38x_quirks(struct platform_device *pdev,
			     struct sdhci_host *host)
{
	struct device_node *np = pdev->dev.of_node;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
	struct resource *res;

#if defined(MY_DEF_HERE)
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mbus");
	pxa->mbus_win_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pxa->mbus_win_regs))
		return PTR_ERR(pxa->mbus_win_regs);
#endif /* MY_DEF_HERE */

	host->quirks &= ~SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN;
	host->quirks |= SDHCI_QUIRK_MISSING_CAPS;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "conf-sdio3");
	if (res) {
		pxa->sdio3_conf_reg = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pxa->sdio3_conf_reg))
			return PTR_ERR(pxa->sdio3_conf_reg);
	} else {
		/*
		 * According to erratum 'FE-2946959' both SDR50 and DDR50
		 * modes require specific clock adjustments in SDIO3
		 * Configuration register, if the adjustment is not done,
		 * remove them from the capabilities.
		 */
		host->caps1 = sdhci_readl(host, SDHCI_CAPABILITIES_1);
		host->caps1 &= ~(SDHCI_SUPPORT_SDR50 | SDHCI_SUPPORT_DDR50);

		dev_warn(&pdev->dev, "conf-sdio3 register not found: disabling SDR50 and DDR50 modes.\nConsider updating your dtb\n");
	}

	/*
	 * According to erratum 'ERR-7878951' Armada 38x SDHCI
	 * controller has different capabilities than the ones shown
	 * in its registers
	 */
	host->caps = sdhci_readl(host, SDHCI_CAPABILITIES);
	if (of_property_read_bool(np, "no-1-8-v")) {
		host->caps &= ~SDHCI_CAN_VDD_180;
		host->mmc->caps &= ~MMC_CAP_1_8V_DDR;
	} else {
		host->caps &= ~SDHCI_CAN_VDD_330;
	}
	host->caps1 &= ~(SDHCI_SUPPORT_SDR104 | SDHCI_USE_SDR50_TUNING);

#if defined(MY_DEF_HERE)
	if (of_property_read_bool(np, "dat3-cd") &&
	    !of_property_read_bool(np, "broken-cd"))
		pxa->dat3_cd_enabled = true;
#endif /* MY_DEF_HERE */

	return 0;
}

#if defined(MY_DEF_HERE)
static void pxav3_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;

	sdhci_set_clock(host, clock);

	/*
	 * The interface clock enable is also used as control
	 * for the A38x SDIO IP, so it can't be powered down
	 * when using HW-based card detection.
	 */
	if (clock == 0 && pxa->dat3_cd_enabled)
		sdhci_writew(host, SDHCI_CLOCK_INT_EN, SDHCI_CLOCK_CONTROL);
}
#endif /* MY_DEF_HERE */

static void pxav3_reset(struct sdhci_host *host, u8 mask)
{
	struct platform_device *pdev = to_platform_device(mmc_dev(host->mmc));
	struct sdhci_pxa_platdata *pdata = pdev->dev.platform_data;
#if defined(MY_DEF_HERE)
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
	u32 reg_val;
#endif /* MY_DEF_HERE */

	sdhci_reset(host, mask);

	if (mask == SDHCI_RESET_ALL) {
		/*
		 * tune timing of read data/command when crc error happen
		 * no performance impact
		 */
		if (pdata && 0 != pdata->clk_delay_cycles) {
			u16 tmp;

			tmp = readw(host->ioaddr + SD_CLOCK_BURST_SIZE_SETUP);
			tmp |= (pdata->clk_delay_cycles & SDCLK_DELAY_MASK)
				<< SDCLK_DELAY_SHIFT;
			tmp |= SDCLK_SEL;
			writew(tmp, host->ioaddr + SD_CLOCK_BURST_SIZE_SETUP);
		}

#if defined(MY_DEF_HERE)
		if (pxa->dat3_cd_enabled) {
			reg_val = sdhci_readl(host, SD_FIFO_PARAM_REG);
			reg_val |= SD_USE_DAT3 | SD_OVRRD_CLK_OEN |
				   SD_FORCE_CLK_ON;
			sdhci_writel(host, reg_val, SD_FIFO_PARAM_REG);

			/*
			 * For HW detection based on DAT3 pin keep internal
			 * clk switched on after controller reset.
			 */
			reg_val = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
			reg_val |= SDHCI_CLOCK_INT_EN;
			sdhci_writel(host, reg_val, SDHCI_CLOCK_CONTROL);
		}
#endif /* MY_DEF_HERE */
	}
}

#if defined(MY_DEF_HERE)
static void pxav3_init_card(struct sdhci_host *host, struct mmc_card *card)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
	u32 reg_val;

	/*
	 * Armada 38x SDHCI controller requires update of
	 * MMC_CARD bit depending on inserted card type.
	 */
	if (pxa->mbus_win_regs) {
		reg_val = sdhci_readl(host, SD_CE_ATA_1);
		if (mmc_card_mmc(card))
			reg_val |= SDCE_MMC_CARD;
		else
			reg_val &= ~SDCE_MMC_CARD;
		sdhci_writel(host, reg_val, SD_CE_ATA_1);
	}
}
#endif /* MY_DEF_HERE */

#define MAX_WAIT_COUNT 5
static void pxav3_gen_init_74_clocks(struct sdhci_host *host, u8 power_mode)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
	u16 tmp;
	int count;

	if (pxa->power_mode == MMC_POWER_UP
			&& power_mode == MMC_POWER_ON) {

		dev_dbg(mmc_dev(host->mmc),
				"%s: slot->power_mode = %d,"
				"ios->power_mode = %d\n",
				__func__,
				pxa->power_mode,
				power_mode);

		/* set we want notice of when 74 clocks are sent */
		tmp = readw(host->ioaddr + SD_CE_ATA_2);
		tmp |= SDCE_MISC_INT_EN;
		writew(tmp, host->ioaddr + SD_CE_ATA_2);

		/* start sending the 74 clocks */
#if defined(MY_DEF_HERE)
		tmp = readw(host->ioaddr + SD_EXTRA_PARAM_REG);
		tmp |= SDCFG_GEN_PAD_CLK_ON;
		writew(tmp, host->ioaddr + SD_EXTRA_PARAM_REG);
#else /* MY_DEF_HERE */
		tmp = readw(host->ioaddr + SD_CFG_FIFO_PARAM);
		tmp |= SDCFG_GEN_PAD_CLK_ON;
		writew(tmp, host->ioaddr + SD_CFG_FIFO_PARAM);
#endif /* MY_DEF_HERE */

		/* slowest speed is about 100KHz or 10usec per clock */
		udelay(740);
		count = 0;

		while (count++ < MAX_WAIT_COUNT) {
			if ((readw(host->ioaddr + SD_CE_ATA_2)
						& SDCE_MISC_INT) == 0)
				break;
			udelay(10);
		}

		if (count == MAX_WAIT_COUNT)
			dev_warn(mmc_dev(host->mmc), "74 clock interrupt not cleared\n");

		/* clear the interrupt bit if posted */
		tmp = readw(host->ioaddr + SD_CE_ATA_2);
		tmp |= SDCE_MISC_INT;
		writew(tmp, host->ioaddr + SD_CE_ATA_2);
	}
	pxa->power_mode = power_mode;
}

static void pxav3_set_uhs_signaling(struct sdhci_host *host, unsigned int uhs)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
	u16 ctrl_2;

	/*
	 * Set V18_EN -- UHS modes do not work without this.
	 * does not change signaling voltage
	 */
	ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	/* Select Bus Speed Mode for host */
	ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
	switch (uhs) {
	case MMC_TIMING_UHS_SDR12:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
		break;
	case MMC_TIMING_UHS_SDR25:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
		break;
	case MMC_TIMING_UHS_SDR50:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR50 | SDHCI_CTRL_VDD_180;
		break;
	case MMC_TIMING_UHS_SDR104:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR104 | SDHCI_CTRL_VDD_180;
		break;
	case MMC_TIMING_MMC_DDR52:
	case MMC_TIMING_UHS_DDR50:
		ctrl_2 |= SDHCI_CTRL_UHS_DDR50 | SDHCI_CTRL_VDD_180;
		break;
	}

	/*
	 * Update SDIO3 Configuration register according to erratum
	 * FE-2946959
	 */
	if (pxa->sdio3_conf_reg) {
		u8 reg_val  = readb(pxa->sdio3_conf_reg);

		if (uhs == MMC_TIMING_UHS_SDR50 ||
		    uhs == MMC_TIMING_UHS_DDR50) {
			reg_val &= ~SDIO3_CONF_CLK_INV;
			reg_val |= SDIO3_CONF_SD_FB_CLK;
#if defined(MY_DEF_HERE)
		} else if (uhs == MMC_TIMING_MMC_HS ||
			   uhs == MMC_TIMING_SD_HS) {
#else /* MY_DEF_HERE */
		} else if (uhs == MMC_TIMING_MMC_HS) {
#endif /* MY_DEF_HERE */
			reg_val &= ~SDIO3_CONF_CLK_INV;
			reg_val &= ~SDIO3_CONF_SD_FB_CLK;
		} else {
			reg_val |= SDIO3_CONF_CLK_INV;
			reg_val &= ~SDIO3_CONF_SD_FB_CLK;
		}
		writeb(reg_val, pxa->sdio3_conf_reg);
	}

	sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
	dev_dbg(mmc_dev(host->mmc),
		"%s uhs = %d, ctrl_2 = %04X\n",
		__func__, uhs, ctrl_2);
}

static const struct sdhci_ops pxav3_sdhci_ops = {
#if defined(MY_DEF_HERE)
	.set_clock = pxav3_set_clock,
#else /* MY_DEF_HERE */
	.set_clock = sdhci_set_clock,
#endif /* MY_DEF_HERE */
	.platform_send_init_74_clocks = pxav3_gen_init_74_clocks,
	.get_max_clock = sdhci_pltfm_clk_get_max_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = pxav3_reset,
	.set_uhs_signaling = pxav3_set_uhs_signaling,
#if defined(MY_DEF_HERE)
	.init_card = pxav3_init_card,
#endif /* MY_DEF_HERE */
};

static struct sdhci_pltfm_data sdhci_pxav3_pdata = {
	.quirks = SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK
		| SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC
		| SDHCI_QUIRK_32BIT_ADMA_SIZE
		| SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.ops = &pxav3_sdhci_ops,
};

#ifdef CONFIG_OF
static const struct of_device_id sdhci_pxav3_of_match[] = {
	{
		.compatible = "mrvl,pxav3-mmc",
	},
	{
		.compatible = "marvell,armada-380-sdhci",
	},
	{},
};
MODULE_DEVICE_TABLE(of, sdhci_pxav3_of_match);

static struct sdhci_pxa_platdata *pxav3_get_mmc_pdata(struct device *dev)
{
	struct sdhci_pxa_platdata *pdata;
	struct device_node *np = dev->of_node;
	u32 clk_delay_cycles;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (!of_property_read_u32(np, "mrvl,clk-delay-cycles",
				  &clk_delay_cycles))
		pdata->clk_delay_cycles = clk_delay_cycles;

	return pdata;
}
#else
static inline struct sdhci_pxa_platdata *pxav3_get_mmc_pdata(struct device *dev)
{
	return NULL;
}
#endif

static int sdhci_pxav3_probe(struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_pxa_platdata *pdata = pdev->dev.platform_data;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct sdhci_host *host = NULL;
	struct sdhci_pxa *pxa = NULL;
	const struct of_device_id *match;
	int ret;

	pxa = devm_kzalloc(&pdev->dev, sizeof(struct sdhci_pxa), GFP_KERNEL);
	if (!pxa)
		return -ENOMEM;

	host = sdhci_pltfm_init(pdev, &sdhci_pxav3_pdata, 0);
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	pltfm_host->priv = pxa;

	pxa->clk_io = devm_clk_get(dev, "io");
	if (IS_ERR(pxa->clk_io))
		pxa->clk_io = devm_clk_get(dev, NULL);
	if (IS_ERR(pxa->clk_io)) {
		dev_err(dev, "failed to get io clock\n");
		ret = PTR_ERR(pxa->clk_io);
		goto err_clk_get;
	}
	pltfm_host->clk = pxa->clk_io;
	clk_prepare_enable(pxa->clk_io);

	pxa->clk_core = devm_clk_get(dev, "core");
	if (!IS_ERR(pxa->clk_core))
		clk_prepare_enable(pxa->clk_core);

	/* enable 1/8V DDR capable */
	host->mmc->caps |= MMC_CAP_1_8V_DDR;

	if (of_device_is_compatible(np, "marvell,armada-380-sdhci")) {
		ret = armada_38x_quirks(pdev, host);
		if (ret < 0)
			goto err_mbus_win;
#if defined(MY_DEF_HERE)
		ret = mv_conf_mbus_windows(&pdev->dev, pxa->mbus_win_regs,
					   mv_mbus_dram_info());
#else /* MY_DEF_HERE */
		ret = mv_conf_mbus_windows(pdev, mv_mbus_dram_info());
#endif /* MY_DEF_HERE */
		if (ret < 0)
			goto err_mbus_win;
	}

	match = of_match_device(of_match_ptr(sdhci_pxav3_of_match), &pdev->dev);
	if (match) {
		ret = mmc_of_parse(host->mmc);
		if (ret)
			goto err_of_parse;
		sdhci_get_of_property(pdev);
		pdata = pxav3_get_mmc_pdata(dev);
		pdev->dev.platform_data = pdata;
	} else if (pdata) {
		/* on-chip device */
		if (pdata->flags & PXA_FLAG_CARD_PERMANENT)
			host->mmc->caps |= MMC_CAP_NONREMOVABLE;

		/* If slot design supports 8 bit data, indicate this to MMC. */
		if (pdata->flags & PXA_FLAG_SD_8_BIT_CAPABLE_SLOT)
			host->mmc->caps |= MMC_CAP_8_BIT_DATA;

		if (pdata->quirks)
			host->quirks |= pdata->quirks;
		if (pdata->quirks2)
			host->quirks2 |= pdata->quirks2;
		if (pdata->host_caps)
			host->mmc->caps |= pdata->host_caps;
		if (pdata->host_caps2)
			host->mmc->caps2 |= pdata->host_caps2;
		if (pdata->pm_caps)
			host->mmc->pm_caps |= pdata->pm_caps;

		if (gpio_is_valid(pdata->ext_cd_gpio)) {
			ret = mmc_gpio_request_cd(host->mmc, pdata->ext_cd_gpio,
						  0);
			if (ret) {
				dev_err(mmc_dev(host->mmc),
					"failed to allocate card detect gpio\n");
				goto err_cd_req;
			}
		}
	}

#if defined(MY_DEF_HERE)
	if (!pxa->dat3_cd_enabled) {
#endif /* MY_DEF_HERE */
	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, PXAV3_RPM_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_suspend_ignore_children(&pdev->dev, 1);
#if defined(MY_DEF_HERE)
	}
#endif /* MY_DEF_HERE */

	ret = sdhci_add_host(host);
	if (ret) {
		dev_err(&pdev->dev, "failed to add host\n");
		goto err_add_host;
	}

	platform_set_drvdata(pdev, host);

	if (host->mmc->pm_caps & MMC_PM_WAKE_SDIO_IRQ)
		device_init_wakeup(&pdev->dev, 1);

#if defined(MY_DEF_HERE)
	if (!pxa->dat3_cd_enabled)
#endif /* MY_DEF_HERE */
	pm_runtime_put_autosuspend(&pdev->dev);

	return 0;

err_add_host:
#if defined(MY_DEF_HERE)
	if (!pxa->dat3_cd_enabled) {
#endif /* MY_DEF_HERE */
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
#if defined(MY_DEF_HERE)
	}
#endif /* MY_DEF_HERE */
err_of_parse:
err_cd_req:
err_mbus_win:
	clk_disable_unprepare(pxa->clk_io);
	clk_disable_unprepare(pxa->clk_core);
err_clk_get:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int sdhci_pxav3_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;

#if defined(MY_DEF_HERE)
	if (!pxa->dat3_cd_enabled) {
#endif /* MY_DEF_HERE */
	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);
#if defined(MY_DEF_HERE)
	}
#endif /* MY_DEF_HERE */

	sdhci_remove_host(host, 1);

	clk_disable_unprepare(pxa->clk_io);
	clk_disable_unprepare(pxa->clk_core);

	sdhci_pltfm_free(pdev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int sdhci_pxav3_suspend(struct device *dev)
{
	int ret;
	struct sdhci_host *host = dev_get_drvdata(dev);
#if defined(MY_DEF_HERE)
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
#endif /* MY_DEF_HERE */

#if defined(MY_DEF_HERE)
	if (!pxa->dat3_cd_enabled)
#endif /* MY_DEF_HERE */
	pm_runtime_get_sync(dev);
	ret = sdhci_suspend_host(host);
#if defined(MY_DEF_HERE)
	if (pxa->dat3_cd_enabled)
		return ret;
#endif /* MY_DEF_HERE */

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static int sdhci_pxav3_resume(struct device *dev)
{
	int ret;
	struct sdhci_host *host = dev_get_drvdata(dev);
#if defined(MY_DEF_HERE)
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;

	if (pxa->mbus_win_regs)
		ret = mv_conf_mbus_windows(dev, pxa->mbus_win_regs,
					   mv_mbus_dram_info());
#endif /* MY_DEF_HERE */

#if defined(MY_DEF_HERE)
	if (!pxa->dat3_cd_enabled)
#endif /* MY_DEF_HERE */
	pm_runtime_get_sync(dev);
	ret = sdhci_resume_host(host);
#if defined(MY_DEF_HERE)
	if (pxa->dat3_cd_enabled)
		return ret;
#endif /* MY_DEF_HERE */

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}
#endif

#ifdef CONFIG_PM
static int sdhci_pxav3_runtime_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;
	int ret;

	ret = sdhci_runtime_suspend_host(host);
	if (ret)
		return ret;

	clk_disable_unprepare(pxa->clk_io);
	if (!IS_ERR(pxa->clk_core))
		clk_disable_unprepare(pxa->clk_core);

	return 0;
}

static int sdhci_pxav3_runtime_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_pxa *pxa = pltfm_host->priv;

	clk_prepare_enable(pxa->clk_io);
	if (!IS_ERR(pxa->clk_core))
		clk_prepare_enable(pxa->clk_core);

	return sdhci_runtime_resume_host(host);
}
#endif

#ifdef CONFIG_PM
static const struct dev_pm_ops sdhci_pxav3_pmops = {
	SET_SYSTEM_SLEEP_PM_OPS(sdhci_pxav3_suspend, sdhci_pxav3_resume)
	SET_RUNTIME_PM_OPS(sdhci_pxav3_runtime_suspend,
		sdhci_pxav3_runtime_resume, NULL)
};

#define SDHCI_PXAV3_PMOPS (&sdhci_pxav3_pmops)

#else
#define SDHCI_PXAV3_PMOPS NULL
#endif

static struct platform_driver sdhci_pxav3_driver = {
	.driver		= {
		.name	= "sdhci-pxav3",
		.of_match_table = of_match_ptr(sdhci_pxav3_of_match),
		.pm	= SDHCI_PXAV3_PMOPS,
	},
	.probe		= sdhci_pxav3_probe,
	.remove		= sdhci_pxav3_remove,
};

module_platform_driver(sdhci_pxav3_driver);

MODULE_DESCRIPTION("SDHCI driver for pxav3");
MODULE_AUTHOR("Marvell International Ltd.");
MODULE_LICENSE("GPL v2");