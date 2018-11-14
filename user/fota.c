
#include <c_types.h>
#include <spi_flash.h>
#include <upgrade.h>

#include "debug.h"

#define USER2_ADDRESS			0x81
#define FOTA_CHUNK_SIZE			512
#define FOTA_CHUNK_PER_SECTOR	8
#define SECTOR_SIZE				4096


typedef struct {
	uint16_t len;
	unsigned char data[FOTA_CHUNK_SIZE];
} FotaChunk;


unsigned char sector[SECTOR_SIZE];
LOCAL size_t fota_chunks = 0;


LOCAL void printbinary(unsigned char *data, uint16_t len) {
	int i;
	for (i = 0; i < 8; i++) {
		INFO(".%02X", data[i]);
	}

	for (i = len-9; i < len; i++) {
		INFO(".%02X", data[i]);
	}
	INFO("\r\n");
}


LOCAL void write_sector(uint32_t n) {
	spi_flash_erase_sector(n);
	spi_flash_write(n * SECTOR_SIZE, (uint32_t*)&sector[0], SECTOR_SIZE);
	os_memset(&sector[0], 0, SECTOR_SIZE);
}


void ICACHE_FLASH_ATTR
update_firmware(const char* msg, uint16_t message_len) {
	if (msg[0] == 'R') {
		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
		INFO("REBOOTING\r\n");
		system_upgrade_reboot();
		return;
	}
	if (msg[0] == 'S') {
		fota_chunks = 0;
		os_memset(&sector, 0, SECTOR_SIZE);
		system_upgrade_flag_set(UPGRADE_FLAG_START);
		INFO("FOTA: Start\r\n");
		return;
	}

	size_t olen;
	FotaChunk chunk;
	uint8_t sectornum = fota_chunks / FOTA_CHUNK_PER_SECTOR;
	uint8_t subsector = fota_chunks % FOTA_CHUNK_PER_SECTOR;
	uint32_t sector_addr = USER2_ADDRESS + sectornum;
	if (msg[0] == 'D') {
		os_memset(&chunk, 0, FOTA_CHUNK_SIZE+2);
		easyq_base64_decode((char*)&chunk, FOTA_CHUNK_SIZE+2, &olen, msg+1, 
				message_len-1);
		unsigned char *ad = &sector[0] + FOTA_CHUNK_SIZE * subsector;
		os_memcpy(ad, &chunk.data, chunk.len);
		if (subsector == 7) {
			write_sector(sector_addr);
			INFO("FOTA: %d:%X\r\n", sectornum, sector_addr);
		}
		fota_chunks++;
	} 
	else if (msg[0] == 'F') {
		if (subsector > 0) {
			INFO("FOTA: Writing remaining bytes at: %X\r\n", sector_addr);
			write_sector(sector_addr);
		}
		fota_chunks = 0;
		INFO("FOTA: finish\r\n");
		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
		INFO("REBOOTING\r\n");
		system_upgrade_reboot();
	}
}

