/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "dfu_over_smp.h"

#if !defined(CONFIG_MCUMGR_SMP_BT) || !defined(CONFIG_MCUMGR_CMD_IMG_MGMT) || !defined(CONFIG_MCUMGR_CMD_OS_MGMT)
#error "DFUOverSMP requires MCUMGR module configs enabled"
#endif

#include <zephyr/dfu/mcuboot.h>
#include <img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/smp_bt.h>
#include <os_mgmt/os_mgmt.h>

#include <platform/CHIPDeviceLayer.h>

#include <lib/support/logging/CHIPLogging.h>

using namespace ::chip::DeviceLayer;

constexpr uint16_t kAdvertisingIntervalMinMs = 400;
constexpr uint16_t kAdvertisingIntervalMaxMs = 500;

DFUOverSMP DFUOverSMP::sDFUOverSMP;

void DFUOverSMP::Init(DFUOverSMPRestartAdvertisingHandler startAdvertisingCb)
{
	os_mgmt_register_group();
	img_mgmt_register_group();
	img_mgmt_set_upload_cb(UploadConfirmHandler, NULL);

	memset(&mBleConnCallbacks, 0, sizeof(mBleConnCallbacks));
	mBleConnCallbacks.disconnected = OnBleDisconnect;

	bt_conn_cb_register(&mBleConnCallbacks);

	restartAdvertisingCallback = startAdvertisingCb;

	PlatformMgr().AddEventHandler(ChipEventHandler, 0);
}

void DFUOverSMP::ConfirmNewImage()
{
	/* Check if the image is run in the REVERT mode and eventually
	 * confirm it to prevent reverting on the next boot. */
	if (mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT) {
		if (boot_write_img_confirmed()) {
			ChipLogError(DeviceLayer,
				     "Confirming firmware image failed, it will be reverted on the next boot.");
		} else {
			ChipLogProgress(DeviceLayer, "New firmware image confirmed.");
		}
	}
}

int DFUOverSMP::UploadConfirmHandler(uint32_t offset, uint32_t size, void *arg)
{
	/* For now just print update progress and confirm data chunk without any additional checks. */
	ChipLogProgress(DeviceLayer, "Software update progress %d B / %d B", offset, size);

	return 0;
}

void DFUOverSMP::StartServer()
{
	if (mIsEnabled) {
		ChipLogProgress(DeviceLayer, "Software update is already enabled");
		return;
	}

	int error = smp_bt_register();

	if (error) {
		ChipLogProgress(DeviceLayer, "Failed to start SMP server: %d", error);
		return;
	}

	mIsEnabled = true;
	ChipLogProgress(DeviceLayer, "Enabled software update");

	/* Start SMP advertising only in case CHIPoBLE advertising is not working. */
	if (!ConnectivityMgr().IsBLEAdvertisingEnabled()) {
		StartBLEAdvertising();
	}
}

void DFUOverSMP::StartBLEAdvertising()
{
	if (!mIsEnabled && !mIsAdvertisingEnabled)
		return;

	const char *deviceName = bt_get_name();
	const uint8_t advFlags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;

	bt_data ad[] = { BT_DATA(BT_DATA_FLAGS, &advFlags, sizeof(advFlags)),
			 BT_DATA(BT_DATA_NAME_COMPLETE, deviceName, static_cast<uint8_t>(strlen(deviceName))) };

	int rc;
	bt_le_adv_param advParams = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME,
							 kAdvertisingIntervalMinMs, kAdvertisingIntervalMaxMs, nullptr);

	rc = bt_le_adv_stop();
	if (rc) {
		ChipLogError(DeviceLayer, "SMP advertising stop failed (rc %d)", rc);
	}

	rc = bt_le_adv_start(&advParams, ad, ARRAY_SIZE(ad), NULL, 0);
	if (rc) {
		ChipLogError(DeviceLayer, "SMP advertising start failed (rc %d)", rc);
	} else {
		ChipLogProgress(DeviceLayer, "Started SMP service BLE advertising");
		mIsAdvertisingEnabled = true;
	}
}

void DFUOverSMP::OnBleDisconnect(struct bt_conn *conId, uint8_t reason)
{
	PlatformMgr().LockChipStack();

	/* After BLE disconnect SMP advertising needs to be restarted. Before making it ensure that BLE disconnect was
	 * not triggered by closing CHIPoBLE service connection (in that case CHIPoBLE advertising needs to be
	 * restarted). */
	if (!ConnectivityMgr().IsBLEAdvertisingEnabled() && (ConnectivityMgr().NumBLEConnections() == 0)) {
		sDFUOverSMP.restartAdvertisingCallback();
	}

	PlatformMgr().UnlockChipStack();
}

void DFUOverSMP::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	if (!GetDFUOverSMP().IsEnabled())
		return;

	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Stopped) {
			/* Check if CHIPoBLE advertising was stopped permanently or it just a matter of opened BLE
			 * connection. */
			if (ConnectivityMgr().NumBLEConnections() == 0)
				sDFUOverSMP.restartAdvertisingCallback();
		}
		break;
	case DeviceEventType::kCommissioningComplete:
		/* Check if after closing CHIPoBLE connection advertising is working, if no start SMP advertising. */
		if (!ConnectivityMgr().IsBLEAdvertisingEnabled()) {
			sDFUOverSMP.restartAdvertisingCallback();
		}
		break;
	default:
		break;
	}
}
