/*
 * Copyright (C) 2017 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

/*
 * Copyright (C) 2019 Ricardo Quesada
 * Unijoysticle additions based on the following BlueKitchen's test/example
 * files:
 *   - hid_host_test.c
 *   - hid_device.c
 *   - gap_inquire.c
 *   - hid_device_test.c
 *   - gap_link_keys.c
 */

/* This file controls everything related to Bluetooth, like connections, state,
 * queries, etc. No Bluetooth logic should be placed outside this file. That
 * way, in theory, it should be possible to support USB devices by replacing
 * this file.
 */

#include "uni_bluetooth.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "btstack_config.h"

#include "uni_debug.h"
#include "uni_hid_device.h"
#include "uni_hid_parser.h"
#include "uni_platform.h"

#define INQUIRY_INTERVAL 5
#define MAX_ATTRIBUTE_VALUE_SIZE 512  // Apparently PS4 has a 470-bytes report
#define MTU 100

// globals
// SDP
static uint8_t attribute_value[MAX_ATTRIBUTE_VALUE_SIZE];
static const unsigned int attribute_value_buffer_size =
    MAX_ATTRIBUTE_VALUE_SIZE;

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t* packet, uint16_t size);
static void handle_sdp_hid_query_result(uint8_t packet_type, uint16_t channel,
                                        uint8_t* packet, uint16_t size);
static void handle_sdp_pid_query_result(uint8_t packet_type, uint16_t channel,
                                        uint8_t* packet, uint16_t size);
static void continue_remote_names(void);
static void start_scan(void);
static void sdp_query_hid_descriptor(uni_hid_device_t* device);
static void sdp_query_product_id(uni_hid_device_t* device);
static void list_link_keys(void);

static void on_l2cap_channel_closed(uint16_t channel, uint8_t* packet,
                                    uint16_t size);
static void on_l2cap_channel_opened(uint16_t channel, uint8_t* packet,
                                    uint16_t size);
static void on_l2cap_incoming_connection(uint16_t channel, uint8_t* packet,
                                         uint16_t size);
static void on_l2cap_data_packet(uint16_t channel, uint8_t* packet,
                                 uint16_t sizel);
static void on_gap_inquiry_result(uint16_t channel, uint8_t* packet,
                                  uint16_t size);
static void on_hci_connection_complete(uint16_t channel, uint8_t* packet,
                                       uint16_t size);
static void on_hci_connection_request(uint16_t channel, uint8_t* packet,
                                      uint16_t size);
static void on_hci_read_remote_version_information_complete(uint16_t channel,
                                                            uint8_t* packet,
                                                            uint16_t size);

static void hid_host_setup(void) {
  // enabled EIR
  hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

  // register for HCI events
  hci_event_callback_registration.callback = &packet_handler;
  hci_add_event_handler(&hci_event_callback_registration);

  l2cap_register_service(packet_handler, PSM_HID_INTERRUPT, MTU, LEVEL_2);
  l2cap_register_service(packet_handler, PSM_HID_CONTROL, MTU, LEVEL_2);

  // Disable stdout buffering
  setbuf(stdout, NULL);
}

// HID results: HID descriptor, PSM interrupt, PSM control, etc.
static void handle_sdp_hid_query_result(uint8_t packet_type, uint16_t channel,
                                        uint8_t* packet, uint16_t size) {
  UNUSED(packet_type);
  UNUSED(channel);
  UNUSED(size);

  des_iterator_t attribute_list_it;
  des_iterator_t additional_des_it;
  uint8_t* des_element;
  uint8_t* element;
  uni_hid_device_t* device;

  device = uni_hid_device_get_current_device();
  if (device == NULL) {
    loge("ERROR: handle_sdp_client_query_result. current device = NULL\n");
    return;
  }

  switch (hci_event_packet_get_type(packet)) {
    case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
      if (sdp_event_query_attribute_byte_get_attribute_length(packet) <=
          attribute_value_buffer_size) {
        attribute_value[sdp_event_query_attribute_byte_get_data_offset(
            packet)] = sdp_event_query_attribute_byte_get_data(packet);
        if ((uint16_t)(sdp_event_query_attribute_byte_get_data_offset(packet) +
                       1) ==
            sdp_event_query_attribute_byte_get_attribute_length(packet)) {
          switch (sdp_event_query_attribute_byte_get_attribute_id(packet)) {
            case BLUETOOTH_ATTRIBUTE_HID_DESCRIPTOR_LIST:
              for (des_iterator_init(&attribute_list_it, attribute_value);
                   des_iterator_has_more(&attribute_list_it);
                   des_iterator_next(&attribute_list_it)) {
                if (des_iterator_get_type(&attribute_list_it) != DE_DES)
                  continue;
                des_element = des_iterator_get_element(&attribute_list_it);
                for (des_iterator_init(&additional_des_it, des_element);
                     des_iterator_has_more(&additional_des_it);
                     des_iterator_next(&additional_des_it)) {
                  if (des_iterator_get_type(&additional_des_it) != DE_STRING)
                    continue;
                  element = des_iterator_get_element(&additional_des_it);
                  const uint8_t* descriptor = de_get_string(element);
                  int descriptor_len = de_get_data_size(element);
                  logi("SDP HID Descriptor (%d):\n", descriptor_len);
                  uni_hid_device_set_hid_descriptor(device, descriptor,
                                                    descriptor_len);
                  printf_hexdump(descriptor, descriptor_len);
                }
              }
              break;
            default:
              break;
          }
        }
      } else {
        loge(
            "SDP attribute value buffer size exceeded: available %d, required "
            "%d\n",
            attribute_value_buffer_size,
            sdp_event_query_attribute_byte_get_attribute_length(packet));
      }
      break;
    case SDP_EVENT_QUERY_COMPLETE:
      sdp_query_product_id(device);
      break;
    default:
      break;
  }
}

// Device ID results: Vendor ID, Product ID, Version, etc...
static void handle_sdp_pid_query_result(uint8_t packet_type, uint16_t channel,
                                        uint8_t* packet, uint16_t size) {
  UNUSED(packet_type);
  UNUSED(channel);
  UNUSED(size);

  uni_hid_device_t* device;
  uint16_t id16;

  device = uni_hid_device_get_current_device();
  if (device == NULL) {
    loge("ERROR: handle_sdp_client_query_result. current device = NULL\n");
    return;
  }

  switch (hci_event_packet_get_type(packet)) {
    case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
      if (sdp_event_query_attribute_byte_get_attribute_length(packet) <=
          attribute_value_buffer_size) {
        attribute_value[sdp_event_query_attribute_byte_get_data_offset(
            packet)] = sdp_event_query_attribute_byte_get_data(packet);
        if ((uint16_t)(sdp_event_query_attribute_byte_get_data_offset(packet) +
                       1) ==
            sdp_event_query_attribute_byte_get_attribute_length(packet)) {
          switch (sdp_event_query_attribute_byte_get_attribute_id(packet)) {
            case BLUETOOTH_ATTRIBUTE_VENDOR_ID:
              if (de_element_get_uint16(attribute_value, &id16))
                uni_hid_device_set_vendor_id(device, id16);
              else
                loge("Error getting vendor id\n");
              break;

            case BLUETOOTH_ATTRIBUTE_PRODUCT_ID:
              if (de_element_get_uint16(attribute_value, &id16))
                uni_hid_device_set_product_id(device, id16);
              else
                loge("Error getting product id\n");
              break;
            default:
              break;
          }
        }
      } else {
        loge(
            "SDP attribute value buffer size exceeded: available %d, required "
            "%d\n",
            attribute_value_buffer_size,
            sdp_event_query_attribute_byte_get_attribute_length(packet));
      }
      break;
    case SDP_EVENT_QUERY_COMPLETE:
      logi("Vendor ID: 0x%04x - Product ID: 0x%04x\n",
           uni_hid_device_get_vendor_id(device),
           uni_hid_device_get_product_id(device));
      uni_hid_device_guess_controller_type(device);
      uni_hid_device_try_assign_joystick_port(device);
      uni_hid_device_set_current_device(NULL);
      break;
  }
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t* packet, uint16_t size) {
  static uint8_t bt_ready = 0;

  uint8_t event;
  bd_addr_t event_addr;
  uni_hid_device_t* device;

  // Ignore all packet events if bt is not ready, with the exception of the "bt
  // is ready" event.
  if ((!bt_ready) &&
      !((packet_type == HCI_EVENT_PACKET) &&
        (hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE))) {
    // printf("Ignoring packet. BT not ready yet\n");
    return;
  }

  switch (packet_type) {
    case HCI_EVENT_PACKET:
      event = hci_event_packet_get_type(packet);
      switch (event) {
        case BTSTACK_EVENT_STATE:
          if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            uni_platform_on_init_complete();
            bt_ready = 1;
            logi("Btstack ready!\n");
            list_link_keys();
            start_scan();
          }
          break;

        // HCI EVENTS
        case HCI_EVENT_COMMAND_COMPLETE: {
          uint16_t opcode =
              hci_event_command_complete_get_command_opcode(packet);
          const uint8_t* param =
              hci_event_command_complete_get_return_parameters(packet);
          uint8_t status = param[0];
          logi("--> HCI_EVENT_COMMAND_COMPLETE. opcode = 0x%04x - status=%d\n",
               opcode, status);
          break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT: {
          uint8_t status = hci_event_authentication_complete_get_status(packet);
          uint16_t handle =
              hci_event_authentication_complete_get_connection_handle(packet);
          logi(
              "--> HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT: status=%d, "
              "handle=0x%04x\n",
              status, handle);
          break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST:
          // inform about pin code request
          log_info("----------------> Pin code request - using '123456'\n");
          logi("----------------> Pin code request - using '123456'\n");
          hci_event_pin_code_request_get_bd_addr(packet, event_addr);
          const char pin_code[] = {0x13, 0x71, 0xda, 0x7d, 0x1a, 0x00};
          hci_send_cmd(&hci_pin_code_request_reply, event_addr,
                       sizeof(pin_code), pin_code);
          break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
          // inform about user confirmation request
          logi("SSP User Confirmation Request with numeric value '%" PRIu32
               "'\n",
               little_endian_read_32(packet, 8));
          logi("SSP User Confirmation Auto accept\n");
          break;
        case HCI_EVENT_HID_META:
          logi("UNSUPPORTED ---> HCI_EVENT_HID_META <---\n");
          break;
        case HCI_EVENT_INQUIRY_RESULT:
          // printf("--> HCI_EVENT_INQUIRY_RESULT <--\n");
          break;
        case HCI_EVENT_CONNECTION_REQUEST:
          logi("--> HCI_EVENT_CONNECTION_REQUEST: link_type = %d <--\n",
               hci_event_connection_request_get_link_type(packet));
          on_hci_connection_request(channel, packet, size);
          break;
        case HCI_EVENT_CONNECTION_COMPLETE:
          logi("--> HCI_EVENT_CONNECTION_COMPLETE:\n");
          on_hci_connection_complete(channel, packet, size);
          break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
          logi("--> HCI_EVENT_DISCONNECTION_COMPLETE\n");
          break;
        case HCI_EVENT_LINK_KEY_REQUEST:
          logi("--> HCI_EVENT_LINK_KEY_REQUEST:\n");
          break;
        case HCI_EVENT_ROLE_CHANGE:
          logi("--> HCI_EVENT_ROLE_CHANGE\n");
          break;
        case HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETE:
          logi("--> HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETE\n");
          break;
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
          // logi("--> HCI_EVENT_INQUIRY_RESULT_WITH_RSSI <--\n");
          break;
        case HCI_EVENT_EXTENDED_INQUIRY_RESPONSE:
          // logi("--> HCI_EVENT_EXTENDED_INQUIRY_RESPONSE <--\n");
          break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
          logi("--> HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE\n");
          reverse_bd_addr(&packet[3], event_addr);
          device = uni_hid_device_get_instance_for_address(event_addr);
          if (device != NULL) {
            if (packet[2] == 0) {
              logi("Name: '%s'\n", &packet[9]);
              uni_hid_device_set_name(device, &packet[9],
                                      strlen((const char*)&packet[9]));
            } else {
              logi("Failed to get name: page timeout\n");
            }
          }
          continue_remote_names();
          break;
        case HCI_EVENT_READ_REMOTE_VERSION_INFORMATION_COMPLETE:
          logi("--> HCI_EVENT_READ_REMOTE_VERSION_INFORMATION_COMPLETE:\n");
          on_hci_read_remote_version_information_complete(channel, packet,
                                                          size);
          break;
        // L2CAP EVENTS
        case L2CAP_EVENT_INCOMING_CONNECTION:
          on_l2cap_incoming_connection(channel, packet, size);
          break;
        case L2CAP_EVENT_CHANNEL_OPENED:
          on_l2cap_channel_opened(channel, packet, size);
          break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
          on_l2cap_channel_closed(channel, packet, size);
          break;

        // GAP EVENTS
        case GAP_EVENT_INQUIRY_RESULT:
          on_gap_inquiry_result(channel, packet, size);
          break;
        case GAP_EVENT_INQUIRY_COMPLETE:
          uni_hid_device_request_inquire();
          continue_remote_names();
          break;
        default:
          break;
      }
      break;
    case L2CAP_DATA_PACKET:
      on_l2cap_data_packet(channel, packet, size);
      break;
    default:
      break;
  }
}
static void on_hci_connection_request(uint16_t channel, uint8_t* packet,
                                      uint16_t size) {
  bd_addr_t event_addr;
  uint32_t cod;
  uni_hid_device_t* device;

  UNUSED(channel);
  UNUSED(size);

  hci_event_connection_request_get_bd_addr(packet, event_addr);
  cod = hci_event_connection_request_get_class_of_device(packet);

  device = uni_hid_device_get_instance_for_address(event_addr);
  if (device == NULL) {
    device = uni_hid_device_create(event_addr);
    if (device == NULL) {
      logi("Cannot create new device... no more slots available\n");
      return;
    }
  }
  uni_hid_device_set_cod(device, cod);
  uni_hid_device_set_incoming(device, 1);
  logi("on_hci_connection_request from: address = %s, cod=0x%04x\n",
       bd_addr_to_str(event_addr), cod);
}

static void on_hci_connection_complete(uint16_t channel, uint8_t* packet,
                                       uint16_t size) {
  bd_addr_t event_addr;
  uni_hid_device_t* device;
  hci_con_handle_t handle;
  uint8_t status;

  UNUSED(channel);
  UNUSED(size);

  hci_event_connection_complete_get_bd_addr(packet, event_addr);
  status = hci_event_connection_complete_get_status(packet);
  if (status) {
    logi("on_hci_connection_complete failed (%d) for %s\n", status,
         bd_addr_to_str(event_addr));
    return;
  }

  device = uni_hid_device_get_instance_for_address(event_addr);
  if (device == NULL) {
    logi("on_hci_connection_complete: failed to get device for %s\n",
         bd_addr_to_str(event_addr));
    return;
  }

  handle = hci_event_connection_complete_get_connection_handle(packet);
  uni_hid_device_set_connection_handle(device, handle);

  if (uni_hid_device_is_incoming(device)) {
    // hci_send_cmd(&hci_authentication_requested, handle);
  }
  // hci_send_cmd(&hci_read_remote_version_information, handle);

  // logi("**** sending auth requested to handle: 0x%04x\n", handle);
  // hci_send_cmd(&hci_authentication_requested, handle);
  gap_request_security_level(handle, LEVEL_1);
}

static void on_hci_read_remote_version_information_complete(uint16_t channel,
                                                            uint8_t* packet,
                                                            uint16_t size) {
  UNUSED(channel);
  UNUSED(size);

  uint8_t status =
      hci_event_read_remote_version_information_complete_get_status(packet);
  if (status != 0) return;

  hci_con_handle_t handle =
      hci_event_read_remote_version_information_complete_get_connection_handle(
          packet);
  uint8_t lmp_ver =
      hci_event_read_remote_version_information_complete_get_version(packet);
  uint16_t mfr_name =
      hci_event_read_remote_version_information_complete_get_manufacturer_name(
          packet);
  uint16_t lmp_subversion =
      hci_event_read_remote_version_information_complete_get_subversion(packet);
  logi("*******  handle=0x%04x, ver=0x%02x, mfr=0x%04x, subver=0x%04x\n",
       handle, lmp_ver, mfr_name, lmp_subversion);
  // hci_send_cmd(&hci_change_connection_packet_type, handle, 0xcc18);
}

static void on_gap_inquiry_result(uint16_t channel, uint8_t* packet,
                                  uint16_t size) {
  const int NAME_LEN_MAX = 240;
  bd_addr_t addr;
  uint8_t status;
  uni_hid_device_t* device;
  uint8_t name_buffer[NAME_LEN_MAX];
  int name_len = -1;

  UNUSED(channel);
  UNUSED(size);

  gap_event_inquiry_result_get_bd_addr(packet, addr);
  uint8_t page_scan_repetition_mode =
      gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
  uint16_t clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
  uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);

  logi("Device found: %s ", bd_addr_to_str(addr));
  logi("with COD: 0x%06x, ", (unsigned int)cod);
  logi("pageScan %d, ", page_scan_repetition_mode);
  logi("clock offset 0x%04x", clock_offset);
  if (gap_event_inquiry_result_get_rssi_available(packet)) {
    logi(", rssi %d dBm", (int8_t)gap_event_inquiry_result_get_rssi(packet));
  }
  if (gap_event_inquiry_result_get_name_available(packet)) {
    name_len = btstack_min(NAME_LEN_MAX - 1,
                           gap_event_inquiry_result_get_name_len(packet));
    memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), name_len);
    name_buffer[name_len] = 0;
    logi(", name '%s'", name_buffer);
  }

  if (uni_hid_device_is_cod_supported(cod)) {
    device = uni_hid_device_get_instance_for_address(addr);
    if (device != NULL && !uni_hid_device_is_orphan(device)) {
      logi("... device already added\n");
      uni_hid_device_dump_device(device);
      return;
    }
    if (!device) device = uni_hid_device_create(addr);
    if (device == NULL) {
      loge("\nError: no more available device slots\n");
      return;
    }
    uni_hid_device_set_cod(device, cod);
    device->page_scan_repetition_mode = page_scan_repetition_mode;
    device->clock_offset = clock_offset;

    if (name_len != -1 && !uni_hid_device_has_name(device)) {
      uni_hid_device_set_name(device, name_buffer, name_len);
    } else {
      uni_hid_device_set_state(device, REMOTE_NAME_REQUEST);
    }

    // Try to establish l2cap channel
    status =
        l2cap_create_channel(packet_handler, device->address, PSM_HID_CONTROL,
                             48, &device->hid_control_cid);
    if (status) {
      loge("\nConnecting or Auth to HID Control failed: 0x%02x", status);
    }
  }
  logi("\n");
}

static void on_l2cap_channel_opened(uint16_t channel, uint8_t* packet,
                                    uint16_t size) {
  uint16_t psm;
  uint8_t status;
  uint16_t local_cid;
  uint16_t remote_cid;
  hci_con_handle_t handle;
  bd_addr_t address;
  uni_hid_device_t* device;
  uint8_t incoming;

  UNUSED(size);

  logi("L2CAP_EVENT_CHANNEL_OPENED (channel=0x%04x)\n", channel);

  l2cap_event_channel_opened_get_address(packet, address);
  status = l2cap_event_channel_opened_get_status(packet);
  if (status) {
    logi("L2CAP Connection failed: 0x%02x.\n", status);
    // Practice showed that if any of these two status are received, it is best
    // to remove the link key. But this is based on empirical evidence, not on
    // theory.
    if (status == L2CAP_CONNECTION_RESPONSE_RESULT_RTX_TIMEOUT ||
        status == L2CAP_CONNECTION_BASEBAND_DISCONNECT) {
      logi("Removing previous link key for address=%s.\n",
           bd_addr_to_str(address));
      uni_hid_device_remove_entry_with_channel(channel);
      // Just in case the key is outdated we remove it. If fixes some
      // l2cap_channel_opened issues. It proves that it works when the status is
      // 0x6a (L2CAP_CONNECTION_BASEBAND_DISCONNECT).
      gap_drop_link_key_for_bd_addr(address);
    }
    return;
  }
  psm = l2cap_event_channel_opened_get_psm(packet);
  local_cid = l2cap_event_channel_opened_get_local_cid(packet);
  remote_cid = l2cap_event_channel_opened_get_remote_cid(packet);
  handle = l2cap_event_channel_opened_get_handle(packet);
  incoming = l2cap_event_channel_opened_get_incoming(packet);
  logi(
      "PSM: 0x%04x, Local CID=0x%04x, Remote CID=0x%04x, handle=0x%04x, "
      "incoming=%d\n",
      psm, local_cid, remote_cid, handle, incoming);

  device = uni_hid_device_get_instance_for_address(address);
  if (device == NULL) {
    loge("could not find device for address\n");
    uni_hid_device_remove_entry_with_channel(channel);
    return;
  }

  uni_hid_device_set_connected(device, true);

  switch (psm) {
    case PSM_HID_CONTROL:
      device->hid_control_cid =
          l2cap_event_channel_opened_get_local_cid(packet);
      logi("HID Control opened, cid 0x%02x\n", device->hid_control_cid);
      break;
    case PSM_HID_INTERRUPT:
      device->hid_interrupt_cid =
          l2cap_event_channel_opened_get_local_cid(packet);
      logi("HID Interrupt opened, cid 0x%02x\n", device->hid_interrupt_cid);
      // Don't request HID descriptor if we already have it.
      if (!uni_hid_device_has_hid_descriptor(device)) {
        sdp_query_hid_descriptor(device);
      }
      break;
    default:
      break;
  }

  if (!uni_hid_device_is_incoming(device)) {
    if (local_cid == 0) {
      loge("local_cid == 0. Abort\n");
      uni_hid_device_remove_entry_with_channel(channel);
      return;
    }
    if (local_cid == device->hid_control_cid) {
      logi("Creating HID INTERRUPT channel\n");
      status = l2cap_create_channel(packet_handler, device->address,
                                    PSM_HID_INTERRUPT, 48,
                                    &device->hid_interrupt_cid);
      if (status) {
        loge("Connecting to HID Control failed: 0x%02x\n", status);
        uni_hid_device_remove_entry_with_channel(channel);
        return;
      }
      logi("New hid interrupt psm = 0x%04x\n", device->hid_interrupt_cid);
    }
    if (local_cid == device->hid_interrupt_cid) {
      logi("HID connection established\n");
    }
  }

  uni_hid_device_try_assign_joystick_port(device);
}

static void on_l2cap_channel_closed(uint16_t channel, uint8_t* packet,
                                    uint16_t size) {
  uint16_t local_cid;
  uni_hid_device_t* device;

  UNUSED(size);

  local_cid = l2cap_event_channel_closed_get_local_cid(packet);
  logi("L2CAP_EVENT_CHANNEL_CLOSED: 0x%04x (channel=0x%04x)\n", local_cid,
       channel);
  device = uni_hid_device_get_instance_for_cid(local_cid);
  if (device == NULL) {
    // Device might already been closed if the Control or Interrupt PSM was
    // closed first.
    logi("Couldn't not find hid_device for cid = 0x%04x\n", local_cid);
    return;
  }
  uni_hid_device_set_connected(device, false);
}

static void on_l2cap_incoming_connection(uint16_t channel, uint8_t* packet,
                                         uint16_t size) {
  bd_addr_t event_addr;
  uni_hid_device_t* device;
  uint16_t local_cid;
  uint16_t remote_cid;
  uint16_t psm;
  hci_con_handle_t handle;

  UNUSED(size);

  psm = l2cap_event_incoming_connection_get_psm(packet);
  handle = l2cap_event_incoming_connection_get_handle(packet);
  local_cid = l2cap_event_incoming_connection_get_local_cid(packet);
  remote_cid = l2cap_event_incoming_connection_get_remote_cid(packet);

  logi(
      "L2CAP_EVENT_INCOMING_CONNECTION (psm=0x%04x, local_cid=0x%04x, "
      "remote_cid=0x%04x, handle=0x%04x, "
      "channel=0x%04x\n",
      psm, local_cid, remote_cid, handle, channel);
  switch (psm) {
    case PSM_HID_CONTROL:
      l2cap_event_incoming_connection_get_address(packet, event_addr);
      device = uni_hid_device_get_instance_for_address(event_addr);
      if (device == NULL) {
        device = uni_hid_device_create(event_addr);
        if (device == NULL) {
          loge("ERROR: no more available free devices\n");
          l2cap_decline_connection(channel);
          break;
        }
      }
      l2cap_accept_connection(channel);
      uni_hid_device_set_connection_handle(device, handle);
      device->hid_control_cid = channel;
      uni_hid_device_set_incoming(device, 1);
      break;
    case PSM_HID_INTERRUPT:
      l2cap_event_incoming_connection_get_address(packet, event_addr);
      device = uni_hid_device_get_instance_for_address(event_addr);
      if (device == NULL) {
        loge("Could not find device for PSM_HID_INTERRUPT = 0x%04x\n", channel);
        l2cap_decline_connection(channel);
        break;
      }
      device->hid_interrupt_cid = channel;
      l2cap_accept_connection(channel);
      break;
    default:
      logi("Unknown PSM = 0x%02x\n", psm);
  }
}

static void on_l2cap_data_packet(uint16_t channel, uint8_t* packet,
                                 uint16_t size) {
  uni_hid_device_t* device;

  device = uni_hid_device_get_instance_for_cid(channel);
  if (device == NULL) {
    loge("Invalid cid: 0x%04x\n", channel);
    return;
  }

  if (channel != device->hid_interrupt_cid) return;

  log_info("PACKET!!");
  printf_hexdump(packet, size);

  if (!uni_hid_device_has_hid_descriptor(device)) {
    logi("Device without HID descriptor yet. Ignoring report\n");
    return;
  }

  if (!uni_hid_device_has_controller_type(device)) {
    logi("Device without a controller type yet. Ignoring report\n");
    return;
  }

  int report_len = size;
  uint8_t* report = packet;

  // check if HID Input Report
  if (report_len < 1) return;
  if (*report != 0xa1) return;
  report++;
  report_len--;

  // In case a joystick port hasn't been assign yet, assign one.
  // uni_hid_device_try_assign_joystick_port(device);

  uni_hid_parser(&device->gamepad, &device->report_parser, report, report_len,
                 device->hid_descriptor, device->hid_descriptor_len);

  // Debug info
  uni_gamepad_dump(&device->gamepad);

  uni_hid_device_process_gamepad(device);
}

static int has_more_remote_name_requests(void) {
  uni_hid_device_t* d;

  d = uni_hid_device_get_first_device_with_state(REMOTE_NAME_REQUEST);
  return (d != NULL);
}

static void do_next_remote_name_request(void) {
  uni_hid_device_t* device;

  device = uni_hid_device_get_first_device_with_state(REMOTE_NAME_REQUEST);
  if (device != NULL) {
    uni_hid_device_set_state(device, REMOTE_NAME_INQUIRED);
    logi("Get remote name of %s...\n", bd_addr_to_str(device->address));
    gap_remote_name_request(device->address, device->page_scan_repetition_mode,
                            device->clock_offset | 0x8000);
  }
}

static void continue_remote_names(void) {
  if (has_more_remote_name_requests()) {
    do_next_remote_name_request();
    return;
  }
  start_scan();
}

static void start_scan(void) {
  logi("Starting inquiry scan..\n");
  gap_inquiry_start(INQUIRY_INTERVAL);
}

static void sdp_query_hid_descriptor(uni_hid_device_t* device) {
  logi("Starting SDP query for HID descriptor for: %s\n",
       bd_addr_to_str(device->address));
  // Needed for the SDP query since it only supports oe SDP query at the time.
  uni_hid_device_t* current = uni_hid_device_get_current_device();
  if (current != NULL) {
    loge(
        "Error: Ouch, another SDP query is in progress (%s) Try again later.\n",
        bd_addr_to_str(current->address));
    return;
  }

  uni_hid_device_set_current_device(device);
  uint8_t status = sdp_client_query_uuid16(
      &handle_sdp_hid_query_result, device->address,
      BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
  if (status != 0) {
    uni_hid_device_set_current_device(NULL);
    loge("Failed to perform sdp query\n");
  }
}

static void sdp_query_product_id(uni_hid_device_t* device) {
  logi("Starting SDP query for product/vendor ID\n");
  uni_hid_device_t* current = uni_hid_device_get_current_device();
  // This query runs after sdp_query_hid_descriptor() so
  // uni_hid_device_get_current_device() must not be NULL
  if (current == NULL) {
    loge("Error: current device is NULL. Should not happen\n");
    return;
  }
  uint8_t status =
      sdp_client_query_uuid16(&handle_sdp_pid_query_result, device->address,
                              BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
  if (status != 0) {
    uni_hid_device_set_current_device(NULL);
    loge("Failed to perform SDP DeviceID query\n");
  }
}

static void list_link_keys(void) {
  bd_addr_t addr;
  link_key_t link_key;
  link_key_type_t type;
  btstack_link_key_iterator_t it;

  int ok = gap_link_key_iterator_init(&it);
  if (!ok) {
    loge("Link key iterator not implemented\n");
    return;
  }
  uint8_t delete_keys = uni_platform_is_button_pressed();
  delete_keys = true;

  if (delete_keys)
    printf("Deleting stored link keys:\n");
  else
    printf("Stored link keys:\n");
  while (gap_link_key_iterator_get_next(&it, addr, link_key, &type)) {
    printf("%s - type %u, key: ", bd_addr_to_str(addr), (int)type);
    printf_hexdump(link_key, 16);
    if (delete_keys) {
      gap_drop_link_key_for_bd_addr(addr);
    }
  }
  printf(".\n");
  gap_link_key_iterator_done(&it);
}

int uni_bluetooth_init(void) {
  // Initialize L2CAP
  l2cap_init();

  hid_host_setup();

  // btstack_stdin_setup(stdin_process);
  // Turn on the device
  hci_set_master_slave_policy(HCI_ROLE_MASTER);
  hci_power_control(HCI_POWER_ON);
  return 0;
}

/* EXAMPLE_END */
