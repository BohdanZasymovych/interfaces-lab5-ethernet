#ifndef CLI_H
#define CLI_H

#include "main.h"
#include <stdbool.h>

extern volatile bool cli_test_colors_requested;
extern volatile bool cli_orientation_toggle_requested;
extern volatile bool cli_get_bme_requested;
extern volatile bool cli_get_color_requested;

void USB_CDC_Send_Async(const uint8_t* data, uint16_t len);
void CDC_CLI_Flush(void);

void Process_CLI_Command(const char* cmd);

// Answers the requests that must not run in the USB interrupt; call from the main loop
void CLI_Process_Pending(void);

#endif
