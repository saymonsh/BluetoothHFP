// BTstack configuration for the Q7 bridge (Windows + WinUSB, Classic only).
// Deliberately minimal: no BLE, no mesh, no A2DP/AVRCP/OBEX, no wide-band codecs,
// and no info logging (info logs would contain AT traffic such as caller numbers).
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

#define HAVE_ASSERT
#define HAVE_BTSTACK_STDIN
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

#define ENABLE_CLASSIC
#define ENABLE_SCO_OVER_HCI
#define ENABLE_LOG_ERROR

#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14

#define NVM_NUM_LINK_KEYS 16

#endif
