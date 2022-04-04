/*
 * Driver for the Microchip/SMSC EMC2301 Fan controller
 *
 * Copyright (C) 2018-2020 Traverse Technologies
 * Author: Mathew McBride <matt@traverse.com.au>
 *
 * Based in part on an earlier implementation by
 * Reinhard Pfau <pfau@gdsys.de>
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 */

//#include <module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/mutex.h>
///#include <linux/thermal.h>

#if LINUX_VERSION_CODE <= KERNEL_VERSION(5, 2, 0)
#define HWMON_CHANNEL_INFO(stype, ...) \
	(&(struct hwmon_channel_info){ .type = hwmon_##stype, .config = (u32[]){ __VA_ARGS__, 0 } })
#endif
/*
 * Factor by equations [2] and [3] from data sheet; valid for fans where the
 * number of edges equals (poles * 2 + 1).
 */
#define FAN_RPM_FACTOR 3932160
#define FAN_TACH_MULTIPLIER 1

#define TACH_HIGH_MASK GENMASK(12, 5)
#define TACH_LOW_MASK GENMASK(4, 0)

#define EMC230X_REG_PRODUCT_ID 0xFD

#define EMC230X_REG_MINTACH 0x38

//#define EMC230X_MAX_COOLING_STATE 7

static bool register_cdev = 1;
module_param(register_cdev, bool, 0);
MODULE_PARM_DESC(register_cdev, "Set to zero to disable registering a cooling device");

struct emc2301_data {
	struct device *dev;
	struct i2c_client *i2c;
	u8 num_fans;
	u16 minimum_rpm[5];
	struct mutex update_lock;
	//u16 min_rpm[5];
	//u16 max_rpm[5];
	u16 setpoint[5];
};

static u16 emc2301_read_fan_tach_int(struct emc2301_data *data, int channel)
{
	struct i2c_client *i2c = data->i2c;

	u8 channel_reg;
	u8 channel_high, channel_low;
	u16 channel_combined;

	channel_reg = 0x3e + (channel * 0x02);
//#if 0
//	dev_dbg(data->dev, "Reading channel %d register %X\n", channel, channel_reg);
//#endif

	channel_high = i2c_smbus_read_byte_data(i2c, channel_reg);

	channel_low = i2c_smbus_read_byte_data(i2c, channel_reg + 0x01);
	channel_combined = ((u16)channel_high << 5) | (channel_low >> 3);

//#if 0
//	dev_dbg(data->dev, "Got values %04X for channel %d\n", channel_combined, channel);
//#endif

	return channel_combined;
}

static u16 emc2301_read_fan_tach(struct device *dev, int channel)
{
	struct emc2301_data *data = dev_get_drvdata(dev);
	return emc2301_read_fan_tach_int(data, channel);
}

static int emc2301_read_fan_rpm(struct device *dev, int channel, long *val)
{
	u16 channel_tach;
	u16 fan_rpm;

	channel_tach = emc2301_read_fan_tach(dev, channel);

	fan_rpm = (FAN_RPM_FACTOR * FAN_TACH_MULTIPLIER) / channel_tach;
	*val = fan_rpm;

	return 0;
}

/* Write a target RPM to the TACH target register
 * This requires RPM speed control to be enabled with
 * EN_ALGO in the fan configuration register.
 */
static int emc2301_set_fan_rpm(struct emc2301_data *devdata, int channel, long target_rpm)
{
	long rpm_high, rpm_low;
	long target_tach;
	u8 channel_reg;

	channel_reg = 0x3c + (channel * 10);

	target_tach = (FAN_RPM_FACTOR * FAN_TACH_MULTIPLIER) / target_rpm;
	//dev_dbg(devdata->dev, "RPM %ld requested, target tach is %ld\n", target_rpm, target_tach);

	rpm_high = (target_tach & TACH_HIGH_MASK) >> 5;
	rpm_low = (target_tach & TACH_LOW_MASK);

//#if 0
//	dev_dbg(devdata->dev, "Writing %02lX %02lX to %02X+%02X\n", rpm_low, rpm_high,
//		channel_reg, channel_reg+1);
//#endif
	mutex_lock(&devdata->update_lock);
	i2c_smbus_write_byte_data(devdata->i2c, channel_reg, rpm_low);
	i2c_smbus_write_byte_data(devdata->i2c, channel_reg + 1, rpm_high);
	
	mutex_unlock(&devdata->update_lock);

	devdata->setpoint[channel] = (u16)target_rpm;

	return 0;
}


static int emc2301_read_fan_fault(struct device *dev, struct i2c_client *i2c, int channel, long *val)
{
	u8 status_reg;

	if (channel > 1) {
		return -EOPNOTSUPP;
	}
	status_reg = i2c_smbus_read_byte_data(i2c, 0x24);
	//dev_dbg(dev, "Channel %d status register %02X\n", channel, status_reg);

	if (status_reg & 0x7) {
		*val = 1;
	} else {
		*val = 0;
	}

	return 0;
}

static int emc2301_read_fan_target(struct emc2301_data *devdata, int channel, long *val)
{
	if (channel > devdata->num_fans) {
		return -EINVAL;
	}
	*val = devdata->setpoint[channel];
	return 0;
}

static int emc2301_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	struct emc2301_data *data = dev_get_drvdata(dev);
	struct i2c_client *i2c = data->i2c;

	if (type != hwmon_fan) {
		return -EOPNOTSUPP;
	}
	if (channel > data->num_fans) {
		return -ENOTSUPP;
	}
	switch (attr) {
	case hwmon_fan_input:
		return emc2301_read_fan_rpm(dev, channel, val);
	case hwmon_fan_target:
		return emc2301_read_fan_target(data, channel, val);
	case hwmon_fan_fault:
		return emc2301_read_fan_fault(dev, i2c, channel, val);
	default:
		return -ENOTSUPP;
		break;
	}

	return 0;
}

static int emc2301_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long val)
{
	struct emc2301_data *data = dev_get_drvdata(dev);

	if (channel > data->num_fans)
		return -EINVAL;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_target:
#if 0
			if (val < data->minimum_rpm[channel]) {
				dev_err(dev, "RPM %ld is lower than channel minimum %ld\n", val, data->minimum_rpm[channel]);
				return -EINVAL;
			}
#endif

			//dev_dbg(dev, "emc2301_write hwmon_fan_target %ld\n", val);
			return emc2301_set_fan_rpm(data, channel, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info *emc2301_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_FAULT | HWMON_F_TARGET), NULL
};

static umode_t emc2301_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel)
{
	//const struct emc2301_data *data = drvdata;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_fault:
			return S_IRUGO;
		case hwmon_fan_target:
			return S_IRUGO | S_IWUSR;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

static const struct hwmon_ops emc2301_ops = {
	.is_visible = emc2301_is_visible,
	.read = emc2301_read,
	.write = emc2301_write,
};

static const struct hwmon_chip_info emc2301_chip_info = {
	.ops = &emc2301_ops,
	.info = emc2301_info,
};

static int emc2301_enable_rpm_control(struct emc2301_data *data, u16 fan_dev, bool enable)
{
	u8 fan_config_reg_addr;
	u8 fan_config_reg_val;
	int ret = 0;

	// get current fan config reg value
	fan_config_reg_addr = 0x32 + (fan_dev * 0x10);
	
	mutex_lock(&data->update_lock);

	fan_config_reg_val = i2c_smbus_read_byte_data(data->i2c, fan_config_reg_addr);

	// update config reg to enable/disable control as requested
	if (enable) {
		// set ENAx to enable drive
		fan_config_reg_val |= (1 << 7);
		// clear RNGx to set minRPM=500
		fan_config_reg_val &= ~(0b11 << 5);
	} else {
		// clear ENAx
		fan_config_reg_val &= ~(1 << 7);
	}

	//dev_dbg(data->dev, "Writing 0x%02X to 0x%02X\n", fan_config_reg_val, fan_config_reg_addr);
	ret = i2c_smbus_write_byte_data(data->i2c, fan_config_reg_addr, fan_config_reg_val);
	if (ret) {
		dev_err(data->dev, "Unable to write fan configuration register %02X\n", fan_config_reg_addr);
		return ret;
	}

	if (!enable) {
		ret = i2c_smbus_write_byte_data(data->i2c, (0x30 + (fan_dev * 0x10)), 0xFF);
	}

	mutex_unlock(&data->update_lock);
	return ret;
}

static int emc2301_i2c_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct device *hwmon_dev;
	struct emc2301_data *data;
	int8_t regval;
	u8 ret;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	data = devm_kzalloc(&i2c->dev, sizeof(struct emc2301_data), GFP_KERNEL);
	if (unlikely(!data))
		return -ENODEV;

	data->dev = &i2c->dev;
	data->i2c = i2c;
	
	mutex_init(&data->update_lock);

	regval = i2c_smbus_read_byte_data(i2c, EMC230X_REG_PRODUCT_ID);
	switch (regval) {
	case 0x34: /* EMC2305 */
		data->num_fans = 5;
		break;
	case 0x35: /* EMC2303 */
		data->num_fans = 3;
		break;
	case 0x36: /* EMC2302 */
		data->num_fans = 2;
		break;
	case 0x37: /* EMC2301 */
		data->num_fans = 1;
		break;
	default:
		dev_err(&i2c->dev, "Unknown product ID %d\n", regval);
		return -ENODEV;
	}
	
	ret = emc2301_enable_rpm_control(data, data->num_fans, true);
	if(ret)
		return ret;	

	dev_info(&i2c->dev, "EMC230%d detected\n", data->num_fans);
	
	//memset(data->min_rpm, 0, sizeof(u16) * ARRAY_SIZE(data->min_rpm));
	//memset(data->max_rpm, 0, sizeof(u16) * ARRAY_SIZE(data->max_rpm));

	hwmon_dev =
		devm_hwmon_device_register_with_info(&i2c->dev, i2c->name, data, &emc2301_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id emc2301_i2c_id[] = { { "emc2305", 0 }, { "emc2304", 0 }, { "emc2303", 0 },
						       { "emc2302", 0 }, { "emc2301", 0 }, {} };

MODULE_DEVICE_TABLE(i2c, emc2301_i2c_id);

static struct i2c_driver emc2301_i2c_driver = {
	.driver = {
		.name = "emc2301",
	},
	.probe    = emc2301_i2c_probe,
	.id_table = emc2301_i2c_id,
};

module_i2c_driver(emc2301_i2c_driver);

MODULE_DESCRIPTION("EMC2301 Fan controller driver");
MODULE_AUTHOR("Mathew McBride <matt@traverse.com.au>");
MODULE_LICENSE("GPL v2");