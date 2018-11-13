
#include <c_types.h>
#include <spi_flash.h>
#include <upgrade.h>

#include "debug.h"



void ICACHE_FLASH_ATTR
update_firmware(const char* msg, uint16_t message_len) {
	//unsigned char data[1024];
	//size_t olen;
	//easyq_base64_decode(data, 1024, &olen, msg, message_len);
	//data[olen] = 0;
	//INFO("BASE64: %s\r\n", data);
	
	system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
	INFO("REBOOTING\r\n");
	system_upgrade_reboot();
}

