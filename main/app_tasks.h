/*
 * Task placement policy for S34GV1.
 * Core 0 keeps network-facing work close to Wi-Fi/LwIP/USB.
 * Core 1 handles local UI, LED indication, and monitoring.
 */

#pragma once

#define APP_CORE_NETWORK 0
#define APP_CORE_APP 1

#define APP_TASK_PRIO_USB_HOST 20
#define APP_TASK_PRIO_USB_CDC 19
#define APP_TASK_PRIO_CELLULAR 5
#define APP_TASK_PRIO_WEB_AUX 5
#define APP_TASK_PRIO_LED 4
#define APP_TASK_PRIO_MONITOR 3
