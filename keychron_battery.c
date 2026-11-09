// SPDX-License-Identifier: GPL-2.0-only
/*
 * HID driver for Keychron mouse battery reporting
 *
 * Copyright (c) 2026 Chris Sutcliff <chris@sutcliff.me>
 *
 * This driver queries battery level from Keychron wireless mice via
 * vendor-specific HID commands. It sends a status request on the control
 * endpoint and picks the response out of the interface's input reports, then
 * exposes the battery level via the power_supply subsystem.
 *
 * Supported devices:
 *   - Keychron M5 (wired mode): 3434:d048
 *   - Keychron M5 (wireless via Ultra-Link 8K receiver): 3434:d028
 */

#include <linux/module.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/delay.h>

#define USB_VENDOR_ID_KEYCHRON		0x3434
#define USB_DEVICE_ID_KEYCHRON_M5	0xd048
#define USB_DEVICE_ID_KEYCHRON_RECV	0xd028

#define KEYCHRON_REPORT_ID_CMD		0xB3
#define KEYCHRON_REPORT_ID_RESP		0xB4
#define KEYCHRON_CMD_STATUS		0x06
#define KEYCHRON_BATTERY_OFFSET		20
#define KEYCHRON_VENDOR_INTERFACE	4

#define KEYCHRON_POLL_INTERVAL_MS	300000	/* 5 minutes */
#define KEYCHRON_RETRY_POLL_MS		60000	/* 1 minute, while mouse not yet responding */
#define KEYCHRON_STALE_MS		900000
#define KEYCHRON_RESPONSE_TIMEOUT_MS	500
#define KEYCHRON_REPORT_SIZE		64
#define KEYCHRON_QUERY_RETRIES		3
#define KEYCHRON_RETRY_DELAY_MS		100

/*
 * Global state for ensuring only one battery instance exists.
 * Multiple HID interfaces probe for the same physical device.
 */
static struct keychron_device *keychron_battery_owner;
static DEFINE_MUTEX(keychron_battery_mutex);

struct keychron_device {
	struct hid_device *hdev;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	struct delayed_work battery_work;
	struct completion response_received;
	int battery_capacity;
	int pending_battery;
	unsigned long last_reading;
	bool owns_battery;
	atomic_t waiting_response;
};

static enum power_supply_property keychron_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int keychron_battery_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct keychron_device *kdev = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = kdev->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (kdev->battery_capacity >= 80)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		else if (kdev->battery_capacity >= 40)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		else if (kdev->battery_capacity >= 10)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Keychron M5";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Keychron";
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int keychron_raw_event(struct hid_device *hdev,
			      struct hid_report *report, u8 *data, int size)
{
	struct keychron_device *kdev = hid_get_drvdata(hdev);

	if (!kdev || !kdev->owns_battery ||
	    !atomic_read(&kdev->waiting_response))
		return 0;

	if (size >= KEYCHRON_BATTERY_OFFSET + 1 &&
	    data[0] == KEYCHRON_REPORT_ID_RESP &&
	    data[1] == KEYCHRON_CMD_STATUS &&
	    data[KEYCHRON_BATTERY_OFFSET] <= 100) {
		kdev->pending_battery = data[KEYCHRON_BATTERY_OFFSET];
		atomic_set(&kdev->waiting_response, 0);
		complete(&kdev->response_received);
	}

	return 0;
}

static int keychron_query_battery_once(struct keychron_device *kdev, u8 *buf)
{
	int ret;

	reinit_completion(&kdev->response_received);
	kdev->pending_battery = -1;
	atomic_set(&kdev->waiting_response, 1);

	memset(buf, 0, KEYCHRON_REPORT_SIZE);
	buf[0] = KEYCHRON_REPORT_ID_CMD;
	buf[1] = KEYCHRON_CMD_STATUS;

	/* Output type, not feature: wValue 0x02B3 is what the firmware answers */
	ret = hid_hw_raw_request(kdev->hdev, KEYCHRON_REPORT_ID_CMD, buf,
				 KEYCHRON_REPORT_SIZE, HID_OUTPUT_REPORT,
				 HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;

	if (wait_for_completion_timeout(&kdev->response_received,
			msecs_to_jiffies(KEYCHRON_RESPONSE_TIMEOUT_MS)) &&
	    kdev->pending_battery >= 0)
		return kdev->pending_battery;

	return -ETIMEDOUT;
}

static int keychron_query_battery(struct keychron_device *kdev)
{
	u8 *buf;
	int ret;
	int attempt;

	buf = kmalloc(KEYCHRON_REPORT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_open(kdev->hdev);
	if (ret) {
		kfree(buf);
		return ret;
	}

	for (attempt = 0; attempt < KEYCHRON_QUERY_RETRIES; attempt++) {
		if (attempt > 0)
			msleep(KEYCHRON_RETRY_DELAY_MS);

		ret = keychron_query_battery_once(kdev, buf);
		atomic_set(&kdev->waiting_response, 0);

		if (ret >= 0)
			break;
	}

	if (ret < 0 && attempt == KEYCHRON_QUERY_RETRIES)
		hid_dbg(kdev->hdev, "battery query failed after %d attempts\n",
			KEYCHRON_QUERY_RETRIES);

	hid_hw_close(kdev->hdev);
	kfree(buf);
	return ret;
}

static int keychron_register_battery(struct keychron_device *kdev)
{
	struct power_supply_config psy_cfg = {};

	kdev->battery_desc.name = "keychron_mouse";
	kdev->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	kdev->battery_desc.properties = keychron_battery_props;
	kdev->battery_desc.num_properties = ARRAY_SIZE(keychron_battery_props);
	kdev->battery_desc.get_property = keychron_battery_get_property;

	psy_cfg.drv_data = kdev;

	kdev->battery = power_supply_register(&kdev->hdev->dev,
					      &kdev->battery_desc, &psy_cfg);
	if (IS_ERR(kdev->battery)) {
		int ret = PTR_ERR(kdev->battery);

		hid_err(kdev->hdev, "failed to register power supply: %d\n", ret);
		kdev->battery = NULL;
		return ret;
	}

	return 0;
}

static void keychron_battery_work(struct work_struct *work)
{
	struct keychron_device *kdev = container_of(work, struct keychron_device,
						    battery_work.work);
	int battery;

	battery = keychron_query_battery(kdev);

	/*
	 * The mouse is wireless and frequently asleep (notably at boot), so a
	 * failed query is expected and transient. Keep polling on a shorter
	 * interval until it responds rather than giving up — otherwise a single
	 * miss leaves the battery unreported for the whole session.
	 */
	if (battery < 0) {
		if (kdev->battery &&
		    time_after(jiffies, kdev->last_reading +
				       msecs_to_jiffies(KEYCHRON_STALE_MS))) {
			power_supply_unregister(kdev->battery);
			kdev->battery = NULL;
			hid_info(kdev->hdev, "no battery reading for %d minutes, unregistering until the mouse responds\n",
				 KEYCHRON_STALE_MS / 60000);
		}
		schedule_delayed_work(&kdev->battery_work,
				      msecs_to_jiffies(KEYCHRON_RETRY_POLL_MS));
		return;
	}

	/* First successful reading: register the power supply now. */
	if (!kdev->battery) {
		if (keychron_register_battery(kdev)) {
			schedule_delayed_work(&kdev->battery_work,
					      msecs_to_jiffies(KEYCHRON_RETRY_POLL_MS));
			return;
		}
		kdev->battery_capacity = battery;
		hid_info(kdev->hdev, "Keychron mouse battery: %d%%\n", battery);
	} else if (battery != kdev->battery_capacity) {
		kdev->battery_capacity = battery;
		power_supply_changed(kdev->battery);
		hid_dbg(kdev->hdev, "battery: %d%%\n", battery);
	}
	kdev->last_reading = jiffies;

	schedule_delayed_work(&kdev->battery_work,
			      msecs_to_jiffies(KEYCHRON_POLL_INTERVAL_MS));
}

static bool keychron_is_vendor_interface(struct hid_device *hdev)
{
	struct usb_interface *intf;

	if (!hid_is_usb(hdev))
		return false;

	intf = to_usb_interface(hdev->dev.parent);
	return intf->cur_altsetting->desc.bInterfaceNumber ==
	       KEYCHRON_VENDOR_INTERFACE;
}

static void keychron_release_battery(struct keychron_device *kdev)
{
	mutex_lock(&keychron_battery_mutex);
	keychron_battery_owner = NULL;
	kdev->owns_battery = false;
	mutex_unlock(&keychron_battery_mutex);
}

static int keychron_probe(struct hid_device *hdev,
			  const struct hid_device_id *id)
{
	struct keychron_device *kdev;
	int ret;
	int battery;

	kdev = devm_kzalloc(&hdev->dev, sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return -ENOMEM;

	kdev->hdev = hdev;
	kdev->battery_capacity = 0;
	kdev->owns_battery = false;
	init_completion(&kdev->response_received);
	atomic_set(&kdev->waiting_response, 0);
	hid_set_drvdata(hdev, kdev);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/*
	 * Start standard interfaces with full connectivity (keyboard, mouse
	 * input devices etc). The vendor interface only needs hidraw — creating
	 * input devices from its HID descriptor exposes keyboard capabilities
	 * that cause UPower to misclassify the battery as a keyboard.
	 */
	if (keychron_is_vendor_interface(hdev))
		ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	else
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	/* Only handle battery on the vendor interface */
	if (!keychron_is_vendor_interface(hdev))
		return 0;

	mutex_lock(&keychron_battery_mutex);
	if (keychron_battery_owner) {
		mutex_unlock(&keychron_battery_mutex);
		return 0;
	}
	keychron_battery_owner = kdev;
	kdev->owns_battery = true;
	mutex_unlock(&keychron_battery_mutex);

	INIT_DELAYED_WORK(&kdev->battery_work, keychron_battery_work);

	hid_device_io_start(hdev);

	/*
	 * Try an initial query so the battery shows up immediately when the
	 * mouse is awake. A failure here is expected for a sleeping wireless
	 * mouse and must NOT abort battery support: the poll worker keeps
	 * retrying and registers the power supply on the first reading.
	 */
	battery = keychron_query_battery(kdev);
	if (battery >= 0) {
		ret = keychron_register_battery(kdev);
		if (ret) {
			keychron_release_battery(kdev);
			hid_hw_stop(hdev);
			return ret;
		}
		kdev->battery_capacity = battery;
		kdev->last_reading = jiffies;
		hid_info(hdev, "Keychron mouse battery: %d%%\n", battery);
		schedule_delayed_work(&kdev->battery_work,
				      msecs_to_jiffies(KEYCHRON_POLL_INTERVAL_MS));
	} else {
		hid_info(hdev, "battery query failed (%d), mouse likely asleep; will keep polling\n",
			 battery);
		schedule_delayed_work(&kdev->battery_work,
				      msecs_to_jiffies(KEYCHRON_RETRY_POLL_MS));
	}

	return 0;
}

static void keychron_remove(struct hid_device *hdev)
{
	struct keychron_device *kdev = hid_get_drvdata(hdev);

	if (kdev && kdev->owns_battery) {
		cancel_delayed_work_sync(&kdev->battery_work);
		if (kdev->battery)
			power_supply_unregister(kdev->battery);
		keychron_release_battery(kdev);
	}

	hid_hw_stop(hdev);
}

static const struct hid_device_id keychron_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_M5) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYCHRON, USB_DEVICE_ID_KEYCHRON_RECV) },
	{ }
};
MODULE_DEVICE_TABLE(hid, keychron_devices);

static struct hid_driver keychron_driver = {
	.name = "keychron",
	.id_table = keychron_devices,
	.probe = keychron_probe,
	.remove = keychron_remove,
	.raw_event = keychron_raw_event,
};
module_hid_driver(keychron_driver);

MODULE_AUTHOR("Chris Sutcliff <chris@sutcliff.me>");
MODULE_DESCRIPTION("HID driver for Keychron mouse battery reporting");
MODULE_LICENSE("GPL");
