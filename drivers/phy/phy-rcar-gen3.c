// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas R-Car Gen3 USB PHY driver
 *
 * Copyright (C) 2018 Marek Vasut <marek.vasut@gmail.com>
 */

#include <clk.h>
#include <div64.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <fdtdec.h>
#include <generic-phy.h>
#include <malloc.h>
#include <reset.h>
#include <syscon.h>
#include <usb.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <power/regulator.h>

/* USB2.0 Host registers (original offset is +0x200) */
#define USB2_INT_ENABLE		0x000
#define USB2_USBCTR		0x00c
#define USB2_SPD_RSM_TIMSET	0x10c
#define USB2_OC_TIMSET		0x110
#define USB2_COMMCTRL		0x600
#define USB2_OBINTSTA		0x604
#define USB2_OBINTEN		0x608
#define USB2_VBCTRL		0x60c
#define USB2_LINECTRL1		0x610
#define USB2_ADPCTRL		0x630

/* INT_ENABLE */
#define USB2_INT_ENABLE_UCOM_INTEN	BIT(3)
#define USB2_INT_ENABLE_USBH_INTB_EN	BIT(2)
#define USB2_INT_ENABLE_USBH_INTA_EN	BIT(1)

/* USBCTR */
#define USB2_USBCTR_PLL_RST		BIT(1)

/* SPD_RSM_TIMSET */
#define USB2_SPD_RSM_TIMSET_INIT	0x014e029b

/* OC_TIMSET */
#define USB2_OC_TIMSET_INIT		0x000209ab

/* COMMCTRL */
#define USB2_COMMCTRL_OTG_PERI		BIT(31)	/* 1 = Peripheral mode */

/* OBINTSTA and OBINTEN */
#define USB2_OBINT_SESSVLDCHG		BIT(12)
#define USB2_OBINT_IDDIGCHG		BIT(11)

/* VBCTRL */
#define USB2_VBCTRL_DRVVBUSSEL		BIT(8)

/* LINECTRL1 */
#define USB2_LINECTRL1_DPRPD_EN		BIT(19)
#define USB2_LINECTRL1_DP_RPD		BIT(18)
#define USB2_LINECTRL1_DMRPD_EN		BIT(17)
#define USB2_LINECTRL1_DM_RPD		BIT(16)

/* ADPCTRL */
#define USB2_ADPCTRL_OTGSESSVLD		BIT(20)
#define USB2_ADPCTRL_IDDIG		BIT(19)
#define USB2_ADPCTRL_IDPULLUP		BIT(5)	/* 1 = ID sampling is enabled */
#define USB2_ADPCTRL_DRVVBUS		BIT(4)

struct rcar_gen3_phy {
	fdt_addr_t	regs;
	struct clk	clk;
	struct udevice	*vbus_supply;
};

static int rcar_gen3_phy_phy_init(struct phy *phy)
{
	struct rcar_gen3_phy *priv = dev_get_priv(phy->dev);

	/* Initialize USB2 part */
	writel(0, priv->regs + USB2_INT_ENABLE);
	writel(USB2_SPD_RSM_TIMSET_INIT, priv->regs + USB2_SPD_RSM_TIMSET);
	writel(USB2_OC_TIMSET_INIT, priv->regs + USB2_OC_TIMSET);

	return 0;
}

static int rcar_gen3_phy_phy_exit(struct phy *phy)
{
	struct rcar_gen3_phy *priv = dev_get_priv(phy->dev);

	writel(0, priv->regs + USB2_INT_ENABLE);

	return 0;
}

static int rcar_gen3_phy_phy_power_on(struct phy *phy)
{
	struct rcar_gen3_phy *priv = dev_get_priv(phy->dev);
	int ret;

	if (priv->vbus_supply) {
		ret = regulator_set_enable(priv->vbus_supply, true);
		if (ret)
			return ret;
	}

	setbits_le32(priv->regs + USB2_USBCTR, USB2_USBCTR_PLL_RST);
	clrbits_le32(priv->regs + USB2_USBCTR, USB2_USBCTR_PLL_RST);

	return 0;
}

static int rcar_gen3_phy_phy_power_off(struct phy *phy)
{
	struct rcar_gen3_phy *priv = dev_get_priv(phy->dev);

	if (!priv->vbus_supply)
		return 0;

	return regulator_set_enable(priv->vbus_supply, false);
}

static int rcar_gen3_phy_phy_set_mode(struct phy *phy, enum phy_mode mode,
				      int submode)
{
	const u32 adpdevmask = USB2_ADPCTRL_IDDIG | USB2_ADPCTRL_OTGSESSVLD;
	struct rcar_gen3_phy *priv = dev_get_priv(phy->dev);
	u32 adpctrl;

	if (mode == PHY_MODE_USB_OTG) {
		if (submode) {
			/* OTG submode is used as initialization indicator */
			writel(USB2_INT_ENABLE_UCOM_INTEN |
			       USB2_INT_ENABLE_USBH_INTB_EN |
			       USB2_INT_ENABLE_USBH_INTA_EN,
			       priv->regs + USB2_INT_ENABLE);
			setbits_le32(priv->regs + USB2_VBCTRL,
				     USB2_VBCTRL_DRVVBUSSEL);
			writel(USB2_OBINT_SESSVLDCHG | USB2_OBINT_IDDIGCHG,
			       priv->regs + USB2_OBINTSTA);
			setbits_le32(priv->regs + USB2_OBINTEN,
				     USB2_OBINT_SESSVLDCHG |
				     USB2_OBINT_IDDIGCHG);
			setbits_le32(priv->regs + USB2_ADPCTRL,
				     USB2_ADPCTRL_IDPULLUP);
			clrsetbits_le32(priv->regs + USB2_LINECTRL1,
					USB2_LINECTRL1_DP_RPD |
					USB2_LINECTRL1_DM_RPD |
					USB2_LINECTRL1_DPRPD_EN |
					USB2_LINECTRL1_DMRPD_EN,
					USB2_LINECTRL1_DPRPD_EN |
					USB2_LINECTRL1_DMRPD_EN);
		}

		adpctrl = readl(priv->regs + USB2_ADPCTRL);
		if ((adpctrl & adpdevmask) == adpdevmask)
			mode = PHY_MODE_USB_DEVICE;
		else
			mode = PHY_MODE_USB_HOST;
	}

	if (mode == PHY_MODE_USB_HOST) {
		clrbits_le32(priv->regs + USB2_COMMCTRL, USB2_COMMCTRL_OTG_PERI);
		setbits_le32(priv->regs + USB2_LINECTRL1,
			     USB2_LINECTRL1_DP_RPD | USB2_LINECTRL1_DM_RPD);
		setbits_le32(priv->regs + USB2_ADPCTRL, USB2_ADPCTRL_DRVVBUS);
	} else if (mode == PHY_MODE_USB_DEVICE) {
		setbits_le32(priv->regs + USB2_COMMCTRL, USB2_COMMCTRL_OTG_PERI);
		clrsetbits_le32(priv->regs + USB2_LINECTRL1,
				USB2_LINECTRL1_DP_RPD | USB2_LINECTRL1_DM_RPD,
				USB2_LINECTRL1_DM_RPD);
		clrbits_le32(priv->regs + USB2_ADPCTRL, USB2_ADPCTRL_DRVVBUS);
	} else {
		dev_err(phy->dev, "Unknown mode %d\n", mode);
		return -EINVAL;
	}

	return 0;
}

static const struct phy_ops rcar_gen3_phy_phy_ops = {
	.init		= rcar_gen3_phy_phy_init,
	.exit		= rcar_gen3_phy_phy_exit,
	.power_on	= rcar_gen3_phy_phy_power_on,
	.power_off	= rcar_gen3_phy_phy_power_off,
	.set_mode	= rcar_gen3_phy_phy_set_mode,
};

static int rcar_gen3_phy_probe(struct udevice *dev)
{
	struct rcar_gen3_phy *priv = dev_get_priv(dev);
	int ret;

	priv->regs = dev_read_addr(dev);
	if (priv->regs == FDT_ADDR_T_NONE)
		return -EINVAL;

	ret = device_get_supply_regulator(dev, "vbus-supply",
					  &priv->vbus_supply);
	if (ret && ret != -ENOENT) {
		pr_err("Failed to get PHY regulator\n");
		return ret;
	}

	/* Enable clock */
	ret = clk_get_by_index(dev, 0, &priv->clk);
	if (ret)
		return ret;

	ret = clk_enable(&priv->clk);
	if (ret)
		return ret;

	return 0;
}

static int rcar_gen3_phy_remove(struct udevice *dev)
{
	struct rcar_gen3_phy *priv = dev_get_priv(dev);

	clk_disable(&priv->clk);

	return 0;
}

static const struct udevice_id rcar_gen3_phy_of_match[] = {
	{ .compatible = "renesas,rcar-gen3-usb2-phy", },
	{ },
};

U_BOOT_DRIVER(rcar_gen3_phy) = {
	.name		= "rcar-gen3-phy",
	.id		= UCLASS_PHY,
	.of_match	= rcar_gen3_phy_of_match,
	.ops		= &rcar_gen3_phy_phy_ops,
	.probe		= rcar_gen3_phy_probe,
	.remove		= rcar_gen3_phy_remove,
	.priv_auto	= sizeof(struct rcar_gen3_phy),
};
