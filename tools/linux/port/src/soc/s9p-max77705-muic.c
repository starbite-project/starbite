// SPDX-License-Identifier: GPL-2.0
/*
 * MAX77705 USB-C/MUIC path switch for the Galaxy S9+ (star2lte).
 *
 * The MAX77705 carries an embedded MCU ("USBC") that owns the Type-C CC
 * logic, BC1.2 charger detection and the D+/D- switches between the
 * connector and the AP. The AP talks to it over I2C at address 0x25 with an
 * opcode protocol: the opcode and its data go into AP_DATAOUT0.. (0x21..),
 * a write to AP_DATAOUT32 (0x41) terminates the command, the MCU answers in
 * AP_DATAIN0.. (0x51..).
 *
 * The switches are not set by the MCU itself. Samsung's MUIC driver sends
 * COMMAND_CONTROL1_WRITE (0x06) with COM_USB (D+ and D- to the AP) whenever
 * a USB host is attached, and its shutdown path resets the MCU
 * (max77705_reset_ic: register 0x80 = 0x0f), so a kernel entered from TWRP
 * finds the switches in their post-reset state and the host sees nothing on
 * D+/D- however well the SoC PHY is configured. This driver does the one
 * thing that is needed: it routes D+/D- to the AP, and repeats that whenever
 * VBUS reappears.
 *
 * Starbite rescue (docs/RESCUE-USB-HOST.md): with starbite,usb-role = "host"
 * (only in the rescue DTB) the driver also gives the port 5 V when, and only
 * when, the MCU's Type-C state machine says the phone is the source:
 *   - at probe, MCU auto-VBUS off (opcode 0x54, data 0x00), as Samsung's
 *     max77705_usbc_probe() does via max77705_usbc_disable_auto_vbus(), so
 *     VBUS is the AP's decision alone;
 *   - every host_poll_ms, CC_STATUS0 (0x0a) CCStat[2:0]: 2 (cc_SOURCE, a
 *     sink with Rd such as an OTG adapter is attached) turns the charger's
 *     OTG boost on through the vbus-supply regulator, anything else turns it
 *     off; 1 (cc_SINK, a charger is attached) always means off. This is
 *     max77705_ccstat_irq_handler() -> max77705_vbus_turn_on_ctrl() of the
 *     vendor, polled instead of interrupt driven;
 *   - never switched on while BC_STATUS.VBUSDet says someone else already
 *     powers VBUS; the regulator itself refuses while CHGIN/WCIN has power;
 *   - a boost that drops by itself (bypass over-current) is retried twice,
 *     5 s apart, then left off until the device is unplugged;
 *   - off on remove, shutdown and when vbus=0 is written (the rescue init
 *     does that before it switches the port to device mode).
 * The MCU firmware is a DRP by default and presents Rp itself on attach; no
 * CC opcode is sent (the vendor sends none for OTG either).
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#define REG_UIC_HW_REV		0x00
#define REG_UIC_FW_REV		0x01
#define REG_UIC_INT		0x02
#define REG_CC_INT		0x03
#define REG_PD_INT		0x04
#define REG_VDM_INT		0x05
#define REG_USBC_STATUS1	0x06
#define REG_USBC_STATUS2	0x07
#define REG_BC_STATUS		0x08
#define  BC_STATUS_VBUSDET		BIT(7)
#define  BC_STATUS_PRCHGTYP_MASK	(0x7 << 3)
#define  BC_STATUS_DCDTMO		BIT(2)
#define  BC_STATUS_CHGTYP_MASK		(0x3 << 0)
#define REG_CC_STATUS0		0x0a
#define REG_CC_STATUS1		0x0b
#define REG_PD_STATUS0		0x0c
#define REG_PD_STATUS1		0x0d
#define REG_UIC_INT_M		0x0e
#define REG_CC_INT_M		0x0f
#define REG_PD_INT_M		0x10
#define REG_VDM_INT_M		0x11
#define REG_AP_DATAOUT0		0x21	/* OPCODE_WRITE */
#define REG_AP_DATAOUT32	0x41	/* OPCODE_WRITE_END */
#define REG_AP_DATAIN0		0x51	/* OPCODE_READ */

#define OPCODE_BC_CTRL1_READ	0x01
#define OPCODE_BC_CTRL1_WRITE	0x02
#define OPCODE_CONTROL1_READ	0x05
#define OPCODE_CONTROL1_WRITE	0x06
#define OPCODE_AUTO_VBUS	0x54	/* vendor max77705_usbc_{en,dis}able_auto_vbus */

/* CC_STATUS0 CCStat[2:0] (vendor enum max77705_cc_pin_state) */
#define CC_STAT_MASK		0x07
#define CC_NO_CONNECTION	0
#define CC_SINK			1	/* phone is the sink: a charger/host is attached */
#define CC_SOURCE		2	/* phone is the source: a device is attached */

/*
 * CONTROL1: NoBCComp[7] RCPS[6] COMP2SW(D+)[5:3] COMN1SW(D-)[2:0]
 * 001 = USB (AP), 011 = UART, 111 = open.
 */
#define COM_USB			0x09
#define COM_OPEN		0x3f

static int s9p_muic_path = COM_USB;
module_param_named(path, s9p_muic_path, int, 0644);
MODULE_PARM_DESC(path, "CONTROL1 switch value to apply (0x09 USB, 0x3f open, -1 none)");

static int s9p_muic_poll_ms = 2000;
module_param_named(poll_ms, s9p_muic_poll_ms, int, 0644);

static int s9p_muic_host_poll_ms = 250;
module_param_named(host_poll_ms, s9p_muic_host_poll_ms, int, 0644);
MODULE_PARM_DESC(host_poll_ms, "host mode: Type-C state poll interval");

/*
 * Host mode only. 1: boost while CCStat says source (default). 0: never
 * (written by the rescue init before device mode). 2: boost whenever no one
 * else powers VBUS, whatever CCStat says -- a first-test fallback in case the
 * MCU never reports cc_SOURCE; only with nothing but a USB device attached.
 */
static int s9p_muic_vbus = 1;
module_param_named(vbus, s9p_muic_vbus, int, 0644);
MODULE_PARM_DESC(vbus, "host mode VBUS: 0 off, 1 on Type-C source (default), 2 forced");

#define VBUS_RETRY_MS		5000
#define VBUS_MAX_FAILS		3

struct s9p_muic {
	struct i2c_client *client;
	struct delayed_work work;
	u8 last_bc;
	bool applied;
	/* host mode (rescue DTB) */
	bool host;
	struct regulator *vbus;
	bool vbus_on;
	int last_cc;
	int fails;
	unsigned long retry_at;
	bool told_ext;
};

static int muic_read(struct s9p_muic *m, u8 reg)
{
	return i2c_smbus_read_byte_data(m->client, reg);
}

static void muic_log_status(struct s9p_muic *m, const char *when)
{
	u8 st[8];
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(st); i++) {
		ret = muic_read(m, REG_USBC_STATUS1 + i);
		st[i] = ret < 0 ? 0xee : ret;
	}
	dev_info(&m->client->dev,
		 "%s: usbc1=%02x usbc2=%02x bc=%02x(vbus=%d chgtyp=%d prchg=%d) cc0=%02x cc1=%02x pd0=%02x pd1=%02x int=%02x/%02x/%02x/%02x\n",
		 when, st[0], st[1], st[2], !!(st[2] & BC_STATUS_VBUSDET),
		 st[2] & BC_STATUS_CHGTYP_MASK,
		 (st[2] & BC_STATUS_PRCHGTYP_MASK) >> 3,
		 st[4], st[5], st[6], st[7],
		 muic_read(m, REG_UIC_INT), muic_read(m, REG_CC_INT),
		 muic_read(m, REG_PD_INT), muic_read(m, REG_VDM_INT));
}

/* max77705_i2c_opcode_write(): opcode + data, then the end marker */
static int muic_opcode_write(struct s9p_muic *m, u8 opcode, const u8 *data,
			     int len)
{
	u8 buf[33];
	int ret;

	if (len > 32)
		return -EMSGSIZE;
	buf[0] = opcode;
	if (len)
		memcpy(buf + 1, data, len);
	ret = i2c_smbus_write_i2c_block_data(m->client, REG_AP_DATAOUT0,
					     len + 1, buf);
	if (ret < 0)
		return ret;
	if (len < 32)
		ret = i2c_smbus_write_byte_data(m->client, REG_AP_DATAOUT32, 0);
	return ret < 0 ? ret : 0;
}

static int muic_opcode_result(struct s9p_muic *m, u8 *out, int len)
{
	return i2c_smbus_read_i2c_block_data(m->client, REG_AP_DATAIN0, len,
					     out);
}

static int muic_set_path(struct s9p_muic *m, u8 val)
{
	u8 res[4] = {};
	int ret;

	ret = muic_opcode_write(m, OPCODE_CONTROL1_WRITE, &val, 1);
	if (ret) {
		dev_err(&m->client->dev, "CONTROL1 write failed: %d\n", ret);
		return ret;
	}
	msleep(20);
	muic_opcode_result(m, res, sizeof(res));
	dev_info(&m->client->dev,
		 "CONTROL1 <= %02x, MCU answered %02x %02x %02x %02x\n",
		 val, res[0], res[1], res[2], res[3]);
	m->applied = true;
	return 0;
}

static void muic_vbus_off(struct s9p_muic *m, const char *why)
{
	if (!m->vbus_on)
		return;
	regulator_disable(m->vbus);
	m->vbus_on = false;
	dev_info(&m->client->dev, "VBUS off (%s)\n", why);
}

/* host mode: one step of the VBUS state machine */
static void muic_host_step(struct s9p_muic *m, int bc)
{
	struct device *dev = &m->client->dev;
	int cc0 = muic_read(m, REG_CC_STATUS0);
	int cc = cc0 < 0 ? -1 : cc0 & CC_STAT_MASK;
	int mode = READ_ONCE(s9p_muic_vbus);
	bool want, ext;
	int ret;

	if (cc != m->last_cc) {
		dev_info(dev, "CC_STATUS0 %02x: CCStat %d -> %d (%s)\n",
			 cc0 & 0xff, m->last_cc, cc,
			 cc == CC_SOURCE ? "source, a device is attached" :
			 cc == CC_SINK ? "sink, a charger/host is attached" :
			 cc == CC_NO_CONNECTION ? "nothing attached" : "other");
		if (cc != CC_SOURCE) {	/* unplugged: a fresh start next time */
			m->fails = 0;
			m->retry_at = 0;
			m->told_ext = false;
		}
		m->last_cc = cc;
	}
	if (!m->vbus)
		return;

	want = (mode == 1 && cc == CC_SOURCE) || mode == 2;
	if (cc == CC_SINK || cc < 0)
		want = false;		/* a charger, or the MCU does not answer */

	/* the boost dropped by itself (bypass over-current, see the charger) */
	if (m->vbus_on && regulator_is_enabled(m->vbus) <= 0) {
		dev_err(dev, "VBUS dropped while on (over-current?)\n");
		muic_vbus_off(m, "dropped");
		m->fails++;
		m->retry_at = jiffies + msecs_to_jiffies(VBUS_RETRY_MS);
	}

	if (!want) {
		muic_vbus_off(m, mode == 0 ? "vbus=0" : cc == CC_SINK ?
			      "a power source is attached" : "detached");
		return;
	}
	if (m->vbus_on)
		return;
	if (m->fails >= VBUS_MAX_FAILS) {
		if (m->fails == VBUS_MAX_FAILS) {
			dev_err(dev, "VBUS failed %d times, off until unplugged\n",
				m->fails);
			m->fails++;
		}
		return;
	}
	if (m->retry_at && time_before(jiffies, m->retry_at))
		return;
	ext = bc >= 0 && (bc & BC_STATUS_VBUSDET);
	if (ext) {		/* a powered hub or Y cable: not ours to drive */
		if (!m->told_ext)
			dev_info(dev, "VBUS already present (BC_STATUS %02x), no boost\n",
				 bc);
		m->told_ext = true;
		return;
	}
	ret = regulator_enable(m->vbus);
	if (ret) {
		dev_err(dev, "VBUS on failed: %d\n", ret);
		m->fails++;
		m->retry_at = jiffies + msecs_to_jiffies(VBUS_RETRY_MS);
		return;
	}
	m->vbus_on = true;
	dev_info(dev, "VBUS on (CCStat %d%s)\n", cc, mode == 2 ? ", forced" : "");
}

static void muic_poll(struct work_struct *work)
{
	struct s9p_muic *m = container_of(work, struct s9p_muic, work.work);
	int bc = muic_read(m, REG_BC_STATUS);
	int ms;

	if (bc >= 0 && (u8)bc != m->last_bc) {
		dev_info(&m->client->dev, "BC_STATUS %02x -> %02x\n",
			 m->last_bc, bc);
		/* VBUS came back: the MCU may have dropped the switch setting */
		if ((bc & BC_STATUS_VBUSDET) && !(m->last_bc & BC_STATUS_VBUSDET)
		    && s9p_muic_path >= 0)
			muic_set_path(m, s9p_muic_path);
		m->last_bc = bc;
	}
	if (m->host)
		muic_host_step(m, bc);
	ms = m->host ? s9p_muic_host_poll_ms : s9p_muic_poll_ms;
	if (ms > 0)
		schedule_delayed_work(&m->work, msecs_to_jiffies(ms));
}

/* host mode setup at probe: auto-VBUS off, the VBUS regulator */
static int muic_host_init(struct s9p_muic *m)
{
	struct device *dev = &m->client->dev;
	u8 off = 0x00, res[2] = {};
	int ret;

	m->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(m->vbus)) {
		ret = PTR_ERR(m->vbus);
		m->vbus = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "host mode without vbus-supply (%d): needs a powered hub\n",
			 ret);
	}

	ret = muic_opcode_write(m, OPCODE_AUTO_VBUS, &off, 1);
	if (!ret) {
		msleep(20);
		muic_opcode_result(m, res, sizeof(res));
	}
	dev_info(dev, "host mode: MCU auto-VBUS off (%d, answer %02x %02x), vbus=%d\n",
		 ret, res[0], res[1], s9p_muic_vbus);
	m->last_cc = -2;	/* log the first state */
	return 0;
}

static int s9p_muic_probe(struct i2c_client *client)
{
	struct s9p_muic *m;
	int hw, fw, bc;

	m = devm_kzalloc(&client->dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->client = client;
	i2c_set_clientdata(client, m);

	hw = muic_read(m, REG_UIC_HW_REV);
	fw = muic_read(m, REG_UIC_FW_REV);
	if (hw < 0 || fw < 0)
		return dev_err_probe(&client->dev, hw < 0 ? hw : fw,
				     "MAX77705 USBC does not answer\n");
	dev_info(&client->dev, "MAX77705 USBC: HW rev %02x, FW rev %02x\n",
		 hw, fw);
	{
		const char *role;

		m->host = !of_property_read_string(client->dev.of_node,
						   "starbite,usb-role", &role) &&
			  !strcmp(role, "host");
	}
	if (m->host) {
		int ret = muic_host_init(m);

		if (ret)
			return dev_err_probe(&client->dev, ret, "host mode\n");
	}
	muic_log_status(m, "at probe");

	bc = muic_read(m, REG_BC_STATUS);
	m->last_bc = bc < 0 ? 0 : bc;

	if (s9p_muic_path >= 0) {
		muic_set_path(m, s9p_muic_path);
		muic_log_status(m, "after switch");
	}

	INIT_DELAYED_WORK(&m->work, muic_poll);
	if (m->host)
		schedule_delayed_work(&m->work, 0);
	else if (s9p_muic_poll_ms > 0)
		schedule_delayed_work(&m->work, msecs_to_jiffies(s9p_muic_poll_ms));
	return 0;
}

static void s9p_muic_remove(struct i2c_client *client)
{
	struct s9p_muic *m = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&m->work);
	muic_vbus_off(m, "remove");
}

static void s9p_muic_shutdown(struct i2c_client *client)
{
	struct s9p_muic *m = i2c_get_clientdata(client);

	if (!m)
		return;
	cancel_delayed_work_sync(&m->work);
	muic_vbus_off(m, "shutdown");
}

static const struct of_device_id s9p_muic_of_match[] = {
	{ .compatible = "samsung,s9p-max77705-muic" },
	{ }
};
MODULE_DEVICE_TABLE(of, s9p_muic_of_match);

static struct i2c_driver s9p_muic_driver = {
	.driver = {
		.name		= "s9p-max77705-muic",
		.of_match_table	= s9p_muic_of_match,
	},
	.probe	= s9p_muic_probe,
	.remove	= s9p_muic_remove,
	.shutdown = s9p_muic_shutdown,
};
module_i2c_driver(s9p_muic_driver);

MODULE_DESCRIPTION("MAX77705 USB-C D+/D- path switch (Galaxy S9+)");
MODULE_LICENSE("GPL");
