#!/usr/bin/env python3
"""Use the controller sleep command on the S9's always-powered touch rails."""
from pathlib import Path
import sys
p = Path(sys.argv[1]) / 'drivers/input/touchscreen/s6sy761.c'
s = p.read_text()
start = s.index('static int s6sy761_suspend(struct device *dev)')
end = s.index('static const struct dev_pm_ops s6sy761_pm_ops', start)
new = '''/* zboot: fixed touch supplies stay on across system suspend. There is no
 * new boot-complete event on resume; use the existing application sleep
 * protocol and preserve runtime-PM state instead of expecting a power cycle. */
static int s6sy761_suspend(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	int ret;

	disable_irq(sdata->client->irq);
	ret = pm_runtime_force_suspend(dev);
	if (ret)
		enable_irq(sdata->client->irq);
	return ret;
}

static int s6sy761_resume(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	int ret = pm_runtime_force_resume(dev);

	if (!ret)
		enable_irq(sdata->client->irq);
	return ret;
}

'''
if 'zboot: fixed touch supplies' not in s:
 s = s[:start] + new + s[end:]
# Probe has already powered the controller up. Without this, force_suspend
# sees the default "suspended" runtime state and skips the sleep command.
anchor = '\tpm_runtime_enable(&client->dev);'
if '\tpm_runtime_set_active(&client->dev);' not in s:
 if s.count(anchor) != 1:
  sys.exit('patch-touch-suspend: runtime-PM probe anchor changed')
 s = s.replace(anchor, '\tpm_runtime_set_active(&client->dev);\n' + anchor)
old_open = '''	return i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
'''
new_open = '''	int ret;

	/* Keep runtime PM aware of an open input device across system sleep. */
	ret = pm_runtime_resume_and_get(&sdata->client->dev);
	if (ret < 0)
		return ret;
	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
	if (ret)
		pm_runtime_put(&sdata->client->dev);
	return ret;
'''
if old_open in s:
 s = s.replace(old_open, new_open, 1)
 close_anchor = '\t\tdev_err(&sdata->client->dev, "failed to turn off sensing\\n");'
 if s.count(close_anchor) != 1:
  sys.exit('patch-touch-suspend: input-close anchor changed')
 s = s.replace(close_anchor, close_anchor + '\n\tpm_runtime_put(&sdata->client->dev);')
# A sleeping S9 controller can leave its level-low interrupt asserted.
# Stop sensing and empty its FIFO before application sleep; masking the Linux
# IRQ alone does not stop the always-on wake controller seeing the line.
runtime_start = s.index('static int s6sy761_runtime_suspend(struct device *dev)')
runtime_end = s.index('/* zboot: fixed touch supplies', runtime_start)
s = s[:runtime_start] + r'''static int s6sy761_runtime_suspend(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	int ret;

	ret = i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_OFF);
	if (ret)
		return ret;
	ret = i2c_smbus_write_byte(sdata->client, S6SY761_CLEAR_EVENT_STACK);
	if (!ret)
		ret = i2c_smbus_write_byte_data(sdata->client,
			S6SY761_APPLICATION_MODE, S6SY761_APP_SLEEP);
	if (ret)
		i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
	return ret;
}

static int s6sy761_runtime_resume(struct device *dev)
{
	struct s6sy761_data *sdata = dev_get_drvdata(dev);
	int ret;

	ret = i2c_smbus_write_byte_data(sdata->client,
		S6SY761_APPLICATION_MODE, S6SY761_APP_NORMAL);
	if (ret)
		return ret;
	return i2c_smbus_write_byte(sdata->client, S6SY761_SENSE_ON);
}

''' + s[runtime_end:]
p.write_text(s)
print('patch-touch-suspend: application sleep with runtime-PM state preserved')
