#pragma once

/*
 * Debug notification system for PS3 XMB
 * Shows popup messages for debugging and development
 */

/* Debug levels */
#define DEBUG_LEVEL_NONE    0
#define DEBUG_LEVEL_ERROR   1
#define DEBUG_LEVEL_INFO    2
#define DEBUG_LEVEL_VERBOSE 3

/* Initialize debug system */
void debug_init(int level);

/* Show XMB popup notification */
void debug_notify(const char *format, ...);

/* Show XMB popup with icon */
void debug_notify_icon(const char *text, int icon_type);

/* Log to file and optionally show popup */
void debug_log(int level, const char *format, ...);

/* Get current debug level */
int debug_get_level(void);

/* Set debug level */
void debug_set_level(int level);

/* Icon types for notifications */
#define NOTIFY_ICON_INFO    0
#define NOTIFY_ICON_CAUTION 1
#define NOTIFY_ICON_FRIEND  2
#define NOTIFY_ICON_SLIDE   3
#define NOTIFY_ICON_TROPHY  4
#define NOTIFY_ICON_PEN     5
#define NOTIFY_ICON_FINGER  6
