// SPDX-License-Identifier: GPL-2.0+
/*
 * drivers/net/phy/siflower.c
 *
 * Driver for Siflower PHYs
 *
 * Copyright (c) 2023 Siflower, Inc.
 *
 * Support : Siflower Phys:
 *		Giga phys: p1211f, p1240
 */
#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/module.h>
#include <linux/delay.h>

#define SIFLOWER_PHY_MODE_SET_ENABLE                            0
#define SIFLOWER_PHY_RXC_DELAY_SET_ENABLE                       0
#define SIFLOWER_PHY_RXC_DELAY_VAL                              0x40
#define SIFLOWER_PHY_TXC_DELAY_VAL                              0x40
#define SIFLOWER_PHY_CLK_OUT_125M_ENABLE                        1

#define SF1211F_PHY_ID                                          0xADB40412
#define SF23P1240_PHY_ID                                          0xADB40411

/* SF1211F PHY LED */
#define SF1211F_EXTREG_LED0                                     0x1E33   // 0
#define SF1211F_EXTREG_LED1                                     0x1E34   // 00101111
#define SF1211F_EXTREG_LED2                                     0x1E35   // 0x40
/* SF1240 PHY BX LED */
#define SF1240_EXTREG_LEDCTRL                                  0x0621
#define SF1240_EXTREG_LED0_1                                   0x0700
#define SF1240_EXTREG_LED0_2                                   0x0701
#define SF1240_EXTREG_LED1_1                                   0x0702
#define SF1240_EXTREG_LED1_2                                   0x0703
#define SF1240_EXTREG_LED2_1                                   0x0706
#define SF1240_EXTREG_LED2_2                                   0x0707
#define SF1240_EXTREG_LED3_1                                   0x0708
#define SF1240_EXTREG_LED3_2                                   0x0709
#define SF1240_EXTREG_LED4_1                                   0x070C
#define SF1240_EXTREG_LED4_2                                   0x070D
#define SF1240_EXTREG_LED5_1                                   0x070E
#define SF1240_EXTREG_LED5_2                                   0x070F
#define SF1240_EXTREG_LED6_1                                   0x0712
#define SF1240_EXTREG_LED6_2                                   0x0713
#define SF1240_EXTREG_LED7_1                                   0x0714
#define SF1240_EXTREG_LED7_2                                   0x0715

/* PHY MODE OPSREG*/
#define SF1211F_EXTREG_GET_PORT_PHY_MODE                        0x062B
#define SF1211F_EXTREG_PHY_MODE_MASK                            0x0070
/* Magic Packet MAC address registers */
#define SIFLOWER_MAGIC_PACKET_MAC_ADDR                          0x0229
/* Magic Packet MAC Passwd registers */
#define SIFLOWER_MAGIC_PACKET_PASSWD_ADDR                       0x022F
#define SIFLOWER_PHY_WOL_PULSE_MODE_SET                         0x062a

/* 8 PHY MODE */
#define SF1211F_EXTREG_PHY_MODE_UTP_TO_RGMII                    0x00
#define SF1211F_EXTREG_PHY_MODE_FIBER_TO_RGMII                  0x10
#define SF1211F_EXTREG_PHY_MODE_UTP_OR_FIBER_TO_RGMII           0x20
#define SF1211F_EXTREG_PHY_MODE_UTP_TO_SGMII                    0x30
#define SF1211F_EXTREG_PHY_MODE_SGMII_PHY_TO_RGMII_MAC          0x40
#define SF1211F_EXTREG_PHY_MODE_SGMII_MAC_TO_RGMII_PHY          0x50
#define SF1211F_EXTREG_PHY_MODE_UTP_TO_FIBER_AUTO               0x60
#define SF1211F_EXTREG_PHY_MODE_UTP_TO_FIBER_FORCE              0x70

/* PHY EXTRW OPSREG */
#define SF1211F_EXTREG_ADDR                                     0x0E
#define SF1211F_EXTREG_DATA                                     0x0D
/* PHY PAGE SPACE */
#define SFPHY_REG_UTP_SPACE                                    0
#define SFPHY_REG_FIBER_SPACE                                  1

/* PHY PAGE SELECT */
#define SF1211F_EXTREG_PHY_MODE_PAGE_SELECT                     0x0016
#define SFPHY_REG_UTP_SPACE_SETADDR                            0x0000
#define SFPHY_REG_FIBER_SPACE_SETADDR                          0x0100
//utp
#define UTP_REG_PAUSE_CAP                                      0x0400    /* Can pause                   */
#define UTP_REG_PAUSE_ASYM                                     0x0800    /* Can pause asymetrically     */
//fiber
#define FIBER_REG_PAUSE_CAP                                    0x0080    /* Can pause                   */
#define FIBER_REG_PAUSE_ASYM                                   0x0100    /* Can pause asymetrically     */

/* specific status register */
#define SIFLOWER_SPEC_REG                                       0x0011

/* Interrupt Enable Register */
#define SIFLOWER_INTR_REG                                       0x0017

/* GET PHY MODE */
#define SFPHY_MODE_CURR                                        sfphy_get_port_type(phydev)

#define SF23P1240_PHY_ADDR_REG		0x1f
#define  SF23P1240_BROADCAST_ADDR	GENMASK(12, 8)
#define  SF23P1240_ADDR_OFFSET		GENMASK(4, 0)

enum siflower_port_type_e
{
	SFPHY_PORT_TYPE_UTP,
	SFPHY_PORT_TYPE_FIBER,
	SFPHY_PORT_TYPE_COMBO,
	SFPHY_PORT_TYPE_EXT
};

static int sf1211f_phy_ext_read(struct phy_device *phydev, u32 regnum)
{
	int ret, val, oldpage = 0, oldval = 0;

	phy_lock_mdio_bus(phydev);

	ret = __phy_read(phydev, SF1211F_EXTREG_ADDR);
	if (ret < 0)
		goto err_handle;
	oldval = ret;

	/* Force change to utp page */
	ret = __phy_read(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT);//get old page
	if (ret < 0)
		goto err_handle;
	oldpage = ret;

	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, SFPHY_REG_UTP_SPACE_SETADDR);
	if (ret < 0)
		goto err_handle;

	/* Default utp ext rw */
	ret = __phy_write(phydev, SF1211F_EXTREG_ADDR, regnum);
	if (ret < 0)
		goto err_handle;

	ret = __phy_read(phydev, SF1211F_EXTREG_DATA);
	if (ret < 0)
		goto err_handle;
	val = ret;

	/* Recover to old page */
	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, oldpage);
	if (ret < 0)
		goto err_handle;

	ret = __phy_write(phydev, SF1211F_EXTREG_ADDR, oldval);
	if (ret < 0)
		goto err_handle;
	ret = val;

err_handle:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

static int sf1211f_phy_ext_write(struct phy_device *phydev, u32 regnum, u16 val)
{
	int ret, oldpage = 0, oldval = 0;

	phy_lock_mdio_bus(phydev);

	ret = __phy_read(phydev, SF1211F_EXTREG_ADDR);
	if (ret < 0)
		goto err_handle;
	oldval = ret;

	/* Force change to utp page */
	ret = __phy_read(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT); //get old page
	if (ret < 0)
		goto err_handle;
	oldpage = ret;

	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, SFPHY_REG_UTP_SPACE_SETADDR);
	if (ret < 0)
		goto err_handle;

	/* Default utp ext rw */
	ret = __phy_write(phydev, SF1211F_EXTREG_ADDR, regnum);
	if (ret < 0)
		goto err_handle;

	ret = __phy_write(phydev, SF1211F_EXTREG_DATA, val);
	if (ret < 0)
		goto err_handle;

	/* Recover to old page */
	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, oldpage);
	if (ret < 0)
		goto err_handle;

	ret = __phy_write(phydev, SF1211F_EXTREG_ADDR, oldval);
	if (ret < 0)
		goto err_handle;

err_handle:
	phy_unlock_mdio_bus(phydev);
	return ret;

}

static int siflower_phy_select_reg_page(struct phy_device *phydev, int space)
{
	int ret;
	if (space == SFPHY_REG_UTP_SPACE)
		ret = phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, SFPHY_REG_UTP_SPACE_SETADDR);
	else
		ret = phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, SFPHY_REG_FIBER_SPACE_SETADDR);
	return ret;
}

static int siflower_phy_get_reg_page(struct phy_device *phydev)
{
	return phy_read(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT);
}

static int siflower_phy_ext_read(struct phy_device *phydev, u32 regnum)
{
	return sf1211f_phy_ext_read(phydev, regnum);
}


static int siflower_phy_ext_write(struct phy_device *phydev, u32 regnum, u16 val)
{
	return sf1211f_phy_ext_write(phydev, regnum, val);
}

static int sfphy_page_read(struct phy_device *phydev, int page, u32 regnum)
{
	int ret, val, oldpage = 0, oldval = 0;

	phy_lock_mdio_bus(phydev);

	ret = __phy_read(phydev, SF1211F_EXTREG_ADDR);
	if (ret < 0)
		goto err_handle;
	oldval = ret;

	ret = __phy_read(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT);
	if (ret < 0)
		goto err_handle;
	oldpage = ret;

	//Select page
	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, (page << 8));
	if (ret < 0)
		goto err_handle;

	ret = __phy_read(phydev, regnum);
	if (ret < 0)
		goto err_handle;
	val = ret;

	/* Recover to old page */
	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, oldpage);
	if (ret < 0)
		goto err_handle;

	ret = __phy_write(phydev, SF1211F_EXTREG_ADDR, oldval);
	if (ret < 0)
		goto err_handle;
	ret = val;

err_handle:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

static int sfphy_page_write(struct phy_device *phydev, int page, u32 regnum, u16 value)
{
	int ret, oldpage = 0, oldval = 0;

	phy_lock_mdio_bus(phydev);

	ret = __phy_read(phydev, SF1211F_EXTREG_ADDR);
	if (ret < 0)
		goto err_handle;
	oldval = ret;

	ret = __phy_read(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT);
	if (ret < 0)
		goto err_handle;
	oldpage = ret;

	//Select page
	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, (page << 8));
	if(ret<0)
		goto err_handle;

	ret = __phy_write(phydev, regnum, value);
	if(ret<0)
		goto err_handle;

	/* Recover to old page */
	ret = __phy_write(phydev, SF1211F_EXTREG_PHY_MODE_PAGE_SELECT, oldpage);
	if (ret < 0)
		goto err_handle;

	ret = __phy_write(phydev, SF1211F_EXTREG_ADDR, oldval);
	if (ret < 0)
		goto err_handle;

err_handle:
	phy_unlock_mdio_bus(phydev);
	return ret;
}

//get port type
static int sfphy_get_port_type(struct phy_device *phydev)
{
	int ret, mode;

	ret = siflower_phy_ext_read(phydev, SF1211F_EXTREG_GET_PORT_PHY_MODE);
	if (ret < 0)
		return ret;
	ret &= SF1211F_EXTREG_PHY_MODE_MASK;

	if (ret == SF1211F_EXTREG_PHY_MODE_UTP_TO_RGMII ||
		ret == SF1211F_EXTREG_PHY_MODE_UTP_TO_SGMII) {
		mode = SFPHY_PORT_TYPE_UTP;
	} else if (ret == SF1211F_EXTREG_PHY_MODE_FIBER_TO_RGMII ||
		ret == SF1211F_EXTREG_PHY_MODE_SGMII_PHY_TO_RGMII_MAC ||
		ret == SF1211F_EXTREG_PHY_MODE_SGMII_MAC_TO_RGMII_PHY) {
		mode = SFPHY_PORT_TYPE_FIBER;
	} else {
		mode = SFPHY_PORT_TYPE_COMBO;
	}

	return mode;
}

static int sf1211f_led_init(struct phy_device *phydev)
{
	int ret;

	ret = siflower_phy_ext_write(phydev, SF1211F_EXTREG_LED0, 0x00);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1211F_EXTREG_LED1, 0x2F);
	if (ret < 0)
		return ret;

	return siflower_phy_ext_write(phydev, SF1211F_EXTREG_LED2, 0x40);
}

static int sf1240_led_init(struct phy_device *phydev)
{
	int ret;
	// set led put low level
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LEDCTRL, 0x04);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_read(phydev, SF1240_EXTREG_LED0_1);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_read(phydev, SF1240_EXTREG_LED0_2);
	if (ret < 0)
		return ret;


	// set led 0 1
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED0_1, 0x00);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED0_2, 0x08);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED1_1, 0xF0);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED1_2, 0x0E);
	if (ret < 0)
		return ret;

	// set led 2 3
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED2_1, 0x00);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED2_2, 0x18);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED3_1, 0xF0);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED3_2, 0x1E);
	if (ret < 0)
		return ret;

	// set led 4
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED4_1, 0x00);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED4_2, 0x28);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED5_1, 0xFE);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED5_2, 0x2E);
	if (ret < 0)
		return ret;

	// set led 6
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED6_2, 0x00);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED6_2, 0x38);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED7_1, 0xF0);
	if (ret < 0)
		return ret;
	ret = siflower_phy_ext_write(phydev, SF1240_EXTREG_LED7_2, 0x3E);
	if (ret < 0)
		return ret;

	return ret;
}


static int sfphy_restart_aneg(struct phy_device *phydev)
{
	int ret, ctl;

	ctl = sfphy_page_read(phydev, SFPHY_REG_FIBER_SPACE, MII_BMCR);
	if (ctl < 0)
		return ctl;
	ctl |= BMCR_ANENABLE;
	ret = sfphy_page_write(phydev, SFPHY_REG_FIBER_SPACE, MII_BMCR, ctl);
	if (ret < 0)
		return ret;

	return 0;
}

int sf1211f_config_aneg(struct phy_device *phydev)
{
	int ret, phymode, oldpage = 0;

	phymode = SFPHY_MODE_CURR;

	if (phymode == SFPHY_PORT_TYPE_UTP || phymode == SFPHY_PORT_TYPE_COMBO) {
		oldpage = siflower_phy_get_reg_page(phydev);
		if (oldpage < 0)
			return oldpage;
		ret = siflower_phy_select_reg_page(phydev, SFPHY_REG_UTP_SPACE);
		if (ret < 0)
			return ret;
		ret = genphy_config_aneg(phydev);
		if (ret < 0)
			return ret;
		ret = siflower_phy_select_reg_page(phydev, oldpage);
		if (ret < 0)
			return ret;
	}

	if (phymode == SFPHY_PORT_TYPE_FIBER || phymode == SFPHY_PORT_TYPE_COMBO) {
		oldpage = siflower_phy_get_reg_page(phydev);
		if (oldpage < 0)
			return oldpage;
		ret = siflower_phy_select_reg_page(phydev, SFPHY_REG_FIBER_SPACE);
		if (ret < 0)
			return ret;
		if (AUTONEG_ENABLE != phydev->autoneg)
			return genphy_setup_forced(phydev);
		ret = sfphy_restart_aneg(phydev);
		if (ret < 0)
			return ret;
		ret = siflower_phy_select_reg_page(phydev, oldpage);
		if (ret < 0)
			return ret;
	}
	return 0;
}

int sf1211f_aneg_done(struct phy_device *phydev)
{
	int val = 0;

	val = phy_read(phydev, 0x16);

	if (val == SFPHY_REG_FIBER_SPACE_SETADDR) {
		val = phy_read(phydev, 0x1);
		val = phy_read(phydev, 0x1);
		return (val < 0) ? val : (val & BMSR_LSTATUS);
	}

	return genphy_aneg_done(phydev);
}

static int sf1211f_rxc_txc_init(struct phy_device *phydev)
{
	int ret;

	ret = (siflower_phy_ext_read(phydev, SF1211F_EXTREG_GET_PORT_PHY_MODE) &
		SF1211F_EXTREG_PHY_MODE_MASK);
	if (ret < 0)
		return ret;

	if ((ret == SF1211F_EXTREG_PHY_MODE_UTP_TO_SGMII) ||
		(ret == SF1211F_EXTREG_PHY_MODE_UTP_TO_FIBER_AUTO) ||
		(ret == SF1211F_EXTREG_PHY_MODE_UTP_TO_FIBER_FORCE))
		return 0;

	// Init rxc and enable rxc
	if (ret == SF1211F_EXTREG_PHY_MODE_UTP_TO_RGMII) {
		ret = phy_read(phydev, 0x11);
		if ((ret & 0x4) == 0x0) {
			ret = siflower_phy_ext_write(phydev,0x1E0C, 0x17);
			if (ret < 0)
				return ret;
			ret = siflower_phy_ext_write(phydev,0x1E58, 0x00);
			if (ret < 0)
				return ret;
		}
	}

	if (phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID){
		// Init rxc delay
		ret = siflower_phy_ext_write(phydev,0x0282, SIFLOWER_PHY_RXC_DELAY_VAL);
		if (ret < 0)
			return ret;
	}
	else if (phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID){
		// Init txc delay
		ret = siflower_phy_ext_write(phydev,0x0281, SIFLOWER_PHY_TXC_DELAY_VAL);
		if (ret < 0)
			return ret;
	}
	else if(phydev->interface == PHY_INTERFACE_MODE_RGMII_ID){
		ret = siflower_phy_ext_write(phydev,0x0282, SIFLOWER_PHY_RXC_DELAY_VAL);
		if (ret < 0)
			return ret;
		ret = siflower_phy_ext_write(phydev,0x0281, SIFLOWER_PHY_TXC_DELAY_VAL);
		if (ret < 0)
			return ret;
	}

	return ret;
}

static int sf1211f_config_opt(struct phy_device *phydev)
{
	int ret;
	//100M utp optimise
	ret = siflower_phy_ext_write(phydev, 0x0149, 0x84);
	if (ret < 0)
		return ret;

	ret = siflower_phy_ext_write(phydev, 0x014A, 0x86);
	if (ret < 0)
		return ret;

	ret = siflower_phy_ext_write(phydev, 0x023C, 0x81);
	if (ret < 0)
		return ret;

	//1000M utp optimise
	ret = siflower_phy_ext_write(phydev, 0x0184, 0x85);
	if (ret < 0)
		return ret;

	ret = siflower_phy_ext_write(phydev, 0x0185, 0x86);
	if (ret < 0)
		return ret;

	ret = siflower_phy_ext_write(phydev, 0x0186, 0x85);
	if (ret < 0)
		return ret;

	ret = siflower_phy_ext_write(phydev, 0x0187, 0x86);
	if (ret < 0)
		return ret;
	return ret;
}
#if SIFLOWER_PHY_CLK_OUT_125M_ENABLE
static int sf1211f_clkout_init(struct phy_device *phydev)
{
	int ret;

	ret = siflower_phy_ext_write(phydev, 0x0272 , 0x09);

	return ret;
}
#endif

#if SIFLOWER_PHY_MODE_SET_ENABLE
//set mode
static int phy_mode_set(struct phy_device *phydev, u16 phyMode)
{
	int ret, num = 0;

	ret = siflower_phy_ext_read(phydev, 0xC417);
	if (ret < 0)
		return ret;

	ret = (ret & 0xF0) | (0x8 | phyMode);

	ret = siflower_phy_ext_write(phydev, 0xC417, ret);
	if (ret < 0)
		return ret;

	while ((siflower_phy_ext_read(phydev, 0xC415) & 0x07) != phyMode) {
		msleep(10);
		if(++num == 5) {
			printk("Phy Mode Set Time Out!\r\n");
			break;
		}
	}

	while (siflower_phy_ext_read(phydev, 0xC413) != 0) {
		msleep(10);
		if(++num == 10) {
			printk("Phy Mode Set Time Out!\r\n");
			break;
		}
	}

	return 0;
}
#endif

static int sf23p1240_probe(struct phy_device *phydev)
{
	u16 val, phy_offs;
	val = phy_read(phydev, SF23P1240_PHY_ADDR_REG);
	phy_offs = FIELD_GET(SF23P1240_ADDR_OFFSET, val);
	val = phy_read(phydev, MII_BMSR);
	printk("PHY addr %u offs %u base %u BMSR %04x\n", phydev->mdio.addr, phy_offs,  phydev->mdio.addr - phy_offs, val);
	
	return 0;
}

int sf23p1240_config_init(struct phy_device *phydev)
{
	int ret;
		ret = genphy_read_abilities(phydev);
	if (ret < 0)
		return ret;

	/* Datasheet says this chip doesn't support MII_ESTATUS. */
	linkmode_mod_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
			phydev->supported, ESTATUS_1000_TFULL);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
			phydev->advertising, ESTATUS_1000_TFULL);
	return sf1240_led_init(phydev);
}

int sf1211f_config_init(struct phy_device *phydev)
{
	int ret, phymode;

#if SIFLOWER_PHY_MODE_SET_ENABLE
	ret = phy_mode_set(phydev, 0x0);
	if (ret < 0)
		return ret;
#endif
	phymode = SFPHY_MODE_CURR;

	if (phymode == SFPHY_PORT_TYPE_UTP || phymode == SFPHY_PORT_TYPE_COMBO) {
		siflower_phy_select_reg_page(phydev, SFPHY_REG_UTP_SPACE);
		ret = genphy_read_abilities(phydev);
		if (ret < 0)
			return ret;
	} else {
		siflower_phy_select_reg_page(phydev, SFPHY_REG_FIBER_SPACE);
		ret = genphy_read_abilities(phydev);
		if (ret < 0)
			return ret;

		linkmode_mod_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
				phydev->supported, ESTATUS_1000_TFULL);
		linkmode_mod_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
				phydev->advertising, ESTATUS_1000_TFULL);
	}

	ret = sf1211f_rxc_txc_init(phydev);
	if (ret < 0)
		return ret;

	ret = sf1211f_config_opt(phydev);
	if (ret < 0)
		return ret;

#if SIFLOWER_PHY_CLK_OUT_125M_ENABLE
	ret = sf1211f_clkout_init(phydev);
	if (ret < 0)
		return ret;
#endif

	return sf1211f_led_init(phydev);
}

static struct phy_driver sf_phy_drivers[] = {
	{
		PHY_ID_MATCH_EXACT(SF1211F_PHY_ID),
		.name =		"SF1211F Gigabit Ethernet",
		.features =	PHY_GBIT_FEATURES,
		.flags =	PHY_POLL,
		.config_init =	sf1211f_config_init,
		.config_aneg =	sf1211f_config_aneg,
		.aneg_done =	sf1211f_aneg_done,
		.write_mmd =	genphy_write_mmd_unsupported,
		.read_mmd =	genphy_read_mmd_unsupported,
		.suspend =	genphy_suspend,
		.resume =	genphy_resume,
	},

	{
		PHY_ID_MATCH_EXACT(SF23P1240_PHY_ID),
		.name =		"SF23P1240 Gigabit Ethernet",
		.features =	PHY_GBIT_FEATURES,
		.flags =	PHY_POLL,
		.config_init =	sf23p1240_config_init,
		.config_aneg =	genphy_config_aneg,
		.probe =	sf23p1240_probe,
		.write_mmd =	genphy_write_mmd_unsupported,
		.read_mmd =	genphy_read_mmd_unsupported,
		.suspend =	genphy_suspend,
		.resume =	genphy_resume,
	},
};

module_phy_driver(sf_phy_drivers);

static struct mdio_device_id __maybe_unused siflower_phy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(SF1211F_PHY_ID) },
	{ PHY_ID_MATCH_EXACT(SF23P1240_PHY_ID) },
	{},
};

MODULE_DEVICE_TABLE(mdio, siflower_phy_tbl);

MODULE_DESCRIPTION("Siflower PHY driver");
MODULE_LICENSE("GPL");
