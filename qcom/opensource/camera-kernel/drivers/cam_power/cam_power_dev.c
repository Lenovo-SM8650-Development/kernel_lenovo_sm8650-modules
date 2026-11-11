// SPDX-License-Identifier: GPL-2.0-only
/* Lenovo Kirby CCI camera power controls, matched to the CN .298 module. */
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include "cam_sensor_io.h"
#include "cam_cci_dev.h"
#include "cam_power_dev.h"

struct kirby_ldo_setting {
	u8 reg;
	u8 value;
};

/* Port order is the HAL's sequence type minus SENSOR_WL2868C_LDO1.
 * In particular, port 6 is the shared IO supply, and port 7 is LDO2 1.1V.
 */
static const struct kirby_ldo_setting kirby_wl2868c_settings[] = {
	{ 0x03, 0x5a }, { 0x04, 0x5a }, { 0x05, 0xa2 }, { 0x06, 0xa2 },
	{ 0x07, 0xa2 }, { 0x08, 0xa2 }, { 0x09, 0x2c }, { 0x04, 0x4d },
};
static const struct kirby_ldo_setting kirby_et5907_settings[] = {
	{ 0x04, 0x66 }, { 0x05, 0x66 }, { 0x06, 0xa0 }, { 0x07, 0xa0 },
	{ 0x08, 0xa0 }, { 0x09, 0xa0 }, { 0x0a, 0x41 }, { 0x05, 0x55 },
};

struct kirby_camera_power {
	struct device *dev;
	struct camera_io_master io;
	struct cam_sensor_cci_client cci;
	struct regulator_bulk_data vin[2];
	struct regulator *pullup;
	struct gpio_desc *enable;
	const struct kirby_ldo_setting *settings;
	unsigned int users[7];
	u8 voltage[7];
	u8 enable_reg;
	u8 voltage_base;
	bool pullup_on;
};

/* Also serializes removal against sensor power requests. */
static DEFINE_MUTEX(kirby_power_lock);
static struct kirby_camera_power *kirby_power;
static int kirby_power_resources;

static int kirby_power_write(struct kirby_camera_power *power, u8 reg, u8 value)
{
	struct cam_sensor_i2c_reg_array data = {
		.reg_addr = reg,
		.reg_data = value,
	};
	struct cam_sensor_i2c_reg_setting setting = {
		.reg_setting = &data,
		.size = 1,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
		.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE,
	};

	return camera_io_dev_write(&power->io, &setting);
}

static int kirby_power_read(struct kirby_camera_power *power, u8 reg, u32 *value)
{
	return camera_io_dev_read(&power->io, reg, value,
		CAMERA_SENSOR_I2C_TYPE_BYTE, CAMERA_SENSOR_I2C_TYPE_BYTE, false);
}

static int kirby_pullup_enable(struct kirby_camera_power *power)
{
	int rc;

	if (!power->pullup_on) {
		rc = regulator_enable(power->pullup);
		if (rc)
			return rc;
		power->pullup_on = true;
	}
	gpiod_set_value_cansleep(power->enable, 1);
	usleep_range(500, 510);
	return 0;
}

static void kirby_pullup_disable(struct kirby_camera_power *power)
{
	int rc;

	gpiod_set_value_cansleep(power->enable, 0);
	if (power->pullup_on) {
		rc = regulator_disable(power->pullup);
		if (rc)
			dev_err(power->dev, "pullup disable failed: %d\n", rc);
		else
			power->pullup_on = false;
	}
}

int cam_kirby_ldo_power(unsigned int port, bool enable)
{
	struct kirby_camera_power *power;
	const struct kirby_ldo_setting *setting;
	unsigned int channel, i;
	u32 value;
	bool in_use = false;
	int rc;

	if (port >= ARRAY_SIZE(kirby_wl2868c_settings))
		return -EINVAL;
	mutex_lock(&kirby_power_lock);
	power = kirby_power;
	if (!power) {
		rc = -ENODEV;
		goto unlock;
	}
	setting = &power->settings[port];
	channel = setting->reg - power->voltage_base;
	if (!enable && !power->users[channel]) {
		rc = 0;
		goto unlock;
	}
	if (enable && power->users[channel] &&
		power->voltage[channel] != setting->value) {
		rc = -EBUSY;
		goto unlock;
	}
	rc = kirby_pullup_enable(power);
	if (rc)
		goto unlock;
	rc = camera_io_init(&power->io);
	if (rc)
		goto check_idle;
	if (enable) {
		rc = kirby_power_write(power, setting->reg, setting->value);
		if (rc)
			goto release;
	}
	rc = kirby_power_read(power, power->enable_reg, &value);
	if (rc)
		goto release;
	if (enable) {
		value |= BIT(channel);
		if (power->enable_reg == 0x0e)
			value |= BIT(7);
	} else if (power->users[channel] == 1) {
		value &= ~BIT(channel);
	}
	rc = kirby_power_write(power, power->enable_reg, value);
	if (!rc) {
		if (enable) {
			power->users[channel]++;
			power->voltage[channel] = setting->value;
		} else {
			power->users[channel]--;
		}
	}
release:
	camera_io_release(&power->io);
check_idle:
	for (i = 0; i < ARRAY_SIZE(power->users); i++)
		in_use |= power->users[i] != 0;
	if (!in_use)
		kirby_pullup_disable(power);
unlock:
	mutex_unlock(&kirby_power_lock);
	return rc;
}

static int kirby_supply_voltage(struct device *dev, struct regulator *supply,
		const char *property)
{
	u32 voltage[2];
	int rc;

	rc = of_property_read_u32_array(dev->of_node, property, voltage, 2);
	if (rc)
		return rc;
	return regulator_set_voltage(supply, voltage[0], voltage[1]);
}

static int kirby_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *parent;
	struct kirby_camera_power *power;
	u32 index = 0, master = 0, address, id;
	int rc;

	parent = of_get_parent(dev->of_node);
	of_property_read_u32(parent, "cell-index", &index);
	of_node_put(parent);
	if (!cam_cci_get_subdev(index))
		return -EPROBE_DEFER;
	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	power->vin[0].supply = "vin1";
	power->vin[1].supply = "vin2";
	rc = devm_regulator_bulk_get(dev, ARRAY_SIZE(power->vin), power->vin);
	if (rc)
		return rc;
	power->pullup = devm_regulator_get(dev, "vin3");
	if (IS_ERR(power->pullup))
		return PTR_ERR(power->pullup);
	power->enable = devm_gpiod_get(dev, "en", GPIOD_ASIS);
	if (IS_ERR(power->enable))
		return PTR_ERR(power->enable);
	rc = kirby_supply_voltage(dev, power->vin[0].consumer, "vin1-voltage-level");
	if (rc)
		return rc;
	rc = kirby_supply_voltage(dev, power->vin[1].consumer, "vin2-voltage-level");
	if (rc)
		return rc;
	rc = kirby_supply_voltage(dev, power->pullup, "vin3-voltage-level");
	if (rc)
		return rc;
	rc = regulator_bulk_enable(ARRAY_SIZE(power->vin), power->vin);
	if (rc)
		return rc;
	rc = regulator_enable(power->pullup);
	if (rc)
		goto disable_vin;
	power->pullup_on = true;
	rc = gpiod_direction_output(power->enable, 0);
	if (rc)
		goto disable_pullup;
	usleep_range(200, 210);
	gpiod_set_value_cansleep(power->enable, 1);
	usleep_range(1000, 1110);
	of_property_read_u32(dev->of_node, "cci-master", &master);
	power->cci.cci_device = index;
	power->cci.cci_i2c_master = master;
	power->cci.i2c_freq_mode = I2C_FAST_MODE;
	power->cci.sid = 0x2f;
	power->cci.retries = 3;
	power->io.master_type = CCI_MASTER;
	power->io.cci_client = &power->cci;
	/* Stock tries WL2868C at 0x2f, then ET5907 at 0x35 on this CCI node. */
	rc = camera_io_init(&power->io);
	if (rc)
		goto disable_pullup;
	for (address = 0x2f; address <= 0x35; address += 6) {
		power->cci.sid = address;
		rc = kirby_power_read(power, 0, &id);
		if (!rc && address == 0x2f && (id == 0x82 || id == 0x33)) {
			power->settings = kirby_wl2868c_settings;
			power->enable_reg = 0x0e;
			power->voltage_base = 0x03;
			break;
		}
		if (!rc && address == 0x35 && id == 0x01) {
			power->settings = kirby_et5907_settings;
			power->enable_reg = 0x03;
			power->voltage_base = 0x04;
			break;
		}
	}
	camera_io_release(&power->io);
	if (!power->settings) {
		rc = -ENODEV;
		goto disable_pullup;
	}
	mutex_lock(&kirby_power_lock);
	if (kirby_power) {
		mutex_unlock(&kirby_power_lock);
		rc = -EBUSY;
		goto disable_pullup;
	}
	kirby_power = power;
	platform_set_drvdata(pdev, power);
	mutex_unlock(&kirby_power_lock);
	dev_info(dev, "Kirby camera LDO ready: CCI%u master%u SID 0x%x id 0x%x\n",
		index, master, power->cci.sid, id);
	return 0;

disable_pullup:
	kirby_pullup_disable(power);
disable_vin:
	regulator_bulk_disable(ARRAY_SIZE(power->vin), power->vin);
	return rc;
}

static int kirby_power_remove(struct platform_device *pdev)
{
	struct kirby_camera_power *power = platform_get_drvdata(pdev);

	mutex_lock(&kirby_power_lock);
	kirby_power = NULL;
	kirby_pullup_disable(power);
	regulator_bulk_disable(ARRAY_SIZE(power->vin), power->vin);
	mutex_unlock(&kirby_power_lock);
	return 0;
}

static int kirby_power_component_bind(struct device *dev,
		struct device *master, void *data)
{
	int rc;

	if (!devres_open_group(dev, &kirby_power_resources, GFP_KERNEL))
		return -ENOMEM;
	rc = kirby_power_probe(to_platform_device(dev));
	if (rc)
		devres_release_group(dev, &kirby_power_resources);
	else
		devres_close_group(dev, &kirby_power_resources);
	return rc;
}

static void kirby_power_component_unbind(struct device *dev,
		struct device *master, void *data)
{
	kirby_power_remove(to_platform_device(dev));
	devres_release_group(dev, &kirby_power_resources);
}

static const struct component_ops kirby_power_component_ops = {
	.bind = kirby_power_component_bind,
	.unbind = kirby_power_component_unbind,
};

static int kirby_power_platform_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &kirby_power_component_ops);
}

static int kirby_power_platform_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &kirby_power_component_ops);
	return 0;
}

static const struct of_device_id kirby_power_match[] = {
	{ .compatible = "lenovo,wl2868c" },
	{ }
};
MODULE_DEVICE_TABLE(of, kirby_power_match);

struct platform_driver cam_kirby_power_platform_driver = {
	.probe = kirby_power_platform_probe,
	.remove = kirby_power_platform_remove,
	.driver = {
		.name = "kirby-camera-power",
		.of_match_table = kirby_power_match,
	},
};

int cam_kirby_power_driver_init(void)
{
	return platform_driver_register(&cam_kirby_power_platform_driver);
}

void cam_kirby_power_driver_exit(void)
{
	platform_driver_unregister(&cam_kirby_power_platform_driver);
}
