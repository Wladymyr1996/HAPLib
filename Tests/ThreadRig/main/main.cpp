/**
 * @file main.cpp
 * @brief Thread rig bring-up: OpenThread on a fixed test network, plus the CLI.
 *
 * THIS IS A TEST IMAGE. The network name, key and the rest of the dataset below
 * are hardcoded and public, so every board of the bench joins the same network
 * with no commissioning. They are not a default for the library or a product.
 */

#include <cstdio>
#include <cstring>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"

#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"

#include "RigCommands.hpp"
#include "RigThread.hpp"

namespace {

/** Different from every other network the bench has hosted. */
constexpr char kNetworkName[] = "HAPRig";
constexpr uint16_t kPanId = 0x4852;
constexpr uint8_t kChannel = 15;
constexpr uint8_t kNetworkKey[OT_NETWORK_KEY_SIZE] = {
    0x48, 0x41, 0x50, 0x52, 0x69, 0x67, 0x01, 0x23,
    0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x10, 0x32};
constexpr uint8_t kExtendedPanId[OT_EXT_PAN_ID_SIZE] = {
    0x48, 0x41, 0x50, 0x52, 0x69, 0x67, 0x00, 0x01};
constexpr uint8_t kMeshLocalPrefix[OT_MESH_LOCAL_PREFIX_SIZE] = {
    0xfd, 0x48, 0x41, 0x50, 0x52, 0x69, 0x67, 0x00};

/**
 * Brings the node into the rig network. The same dataset is applied on every
 * boot, so a board always rejoins this network whatever it held before.
 */
void joinRigNetwork(otInstance* instance) {
  otOperationalDataset dataset;
  otDatasetCreateNewNetwork(instance, &dataset);

  std::memcpy(dataset.mNetworkKey.m8, kNetworkKey, sizeof(kNetworkKey));
  std::memcpy(dataset.mExtendedPanId.m8, kExtendedPanId,
              sizeof(kExtendedPanId));
  std::memcpy(dataset.mMeshLocalPrefix.m8, kMeshLocalPrefix,
              sizeof(kMeshLocalPrefix));
  std::strcpy(dataset.mNetworkName.m8, kNetworkName);
  dataset.mPanId = kPanId;
  dataset.mChannel = kChannel;
  dataset.mChannelMask = 1u << kChannel;
  dataset.mActiveTimestamp.mSeconds = 1;
  dataset.mActiveTimestamp.mTicks = 0;
  dataset.mActiveTimestamp.mAuthoritative = false;
  dataset.mComponents.mIsActiveTimestampPresent = true;

  otDatasetSetActive(instance, &dataset);
  otIp6SetEnabled(instance, true);
  otThreadSetEnabled(instance, true);
}

/** Everything OpenThread's platform needs before it can start. */
void initPlatform() {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Foreign data (another image's NVS) is not worth keeping on a test board.
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_netif_init());

  esp_vfs_eventfd_config_t eventfd = {};
  eventfd.max_fds = 3;
  ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd));
}

}  // namespace

extern "C" void app_main(void) {
  initPlatform();

  static esp_openthread_config_t config = {};
  config.netif_config = ESP_NETIF_DEFAULT_OPENTHREAD();
  config.platform_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
  config.platform_config.host_config.host_connection_mode =
      HOST_CONNECTION_MODE_NONE;
  config.platform_config.port_config.storage_partition_name = "nvs";
  config.platform_config.port_config.netif_queue_size = 10;
  config.platform_config.port_config.task_queue_size = 10;
  ESP_ERROR_CHECK(esp_openthread_start(&config));

  esp_openthread_lock_acquire(portMAX_DELAY);
  esp_openthread_cli_init();
  joinRigNetwork(esp_openthread_get_instance());
  esp_openthread_lock_release();

  // Open the transport now that OpenThread runs (begin() fails before that).
  // A failure is printed as HAPERR and leaves the CLI usable.
  rig::startThread();

  // The CLI runs on a REPL over the USB port. esp_openthread_cli_create_task()
  // busy-spins on USB-Serial-JTAG and trips the task watchdog, so it is not
  // used: lines are typed as `ot <cli line>`.
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  replConfig.prompt = "hap>";
  replConfig.max_cmdline_length = 256;
  replConfig.task_stack_size = 6144;
  esp_console_dev_usb_serial_jtag_config_t device =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(
      esp_console_new_repl_usb_serial_jtag(&device, &replConfig, &repl));
  ESP_ERROR_CHECK(esp_openthread_cli_console_command_register());

  rig::registerCommands();

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
