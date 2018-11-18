
#include <mem.h>
#include <osapi.h>  
#include <c_types.h>
#include <spi_flash.h>
#include <upgrade.h>
#include <string.h>

#include "debug.h"
#include "partition.h"

// HEADER
#include <ip_addr.h>
#include <espconn.h>

#ifndef FOTA_SECTOR_SIZE
#define FOTA_SECTOR_SIZE	4096
#endif


#ifndef FOTA_CHUNK_SIZE
#define FOTA_CHUNK_SIZE		1024	
#endif

#define FOTA_CHUNKS_PER_SECTOR	4	


LOCAL os_event_t fota_task_queue[1];


struct fota_session {
	struct espconn *tcpconn;
	char *server;
	ip_addr_t ip;
	uint16_t port;
	char * recv_buffer;
	size_t recvbuffer_length;
	uint16_t sector;
	uint16_t chunk_index;
	bool ok;
};
// HEADER END


LOCAL struct fota_session fs;


LOCAL void ICACHE_FLASH_ATTR
_fota_proto_delete() {
    if (fs.tcpconn != NULL) {
        espconn_delete(fs.tcpconn);
        if (fs.tcpconn->proto.tcp)
            os_free(fs.tcpconn->proto.tcp);
        os_free(fs.tcpconn);
        fs.tcpconn = NULL;
    }
	os_free(fs.recv_buffer);
}


LOCAL void 
_fota_write_sector() {
	INFO("W: 0x%05X\r\n", fs.sector * FOTA_SECTOR_SIZE);
	SpiFlashOpResult err;
	
	system_soft_wdt_feed();
	err = spi_flash_erase_sector(fs.sector);
	if (err != SPI_FLASH_RESULT_OK) {
		ERROR("Canot erase flash: %d\r\n", err);
		return;
	}
	system_soft_wdt_feed();
	err = spi_flash_write(fs.sector * FOTA_SECTOR_SIZE, 
			(uint32_t *)fs.recv_buffer, 
			FOTA_SECTOR_SIZE);
	if (err != SPI_FLASH_RESULT_OK) {
		ERROR("Canot write flash: %d\r\n", err);
		return;
	}

	system_soft_wdt_feed();
	os_memset(fs.recv_buffer, 0, FOTA_SECTOR_SIZE);
}


LOCAL void
_fota_get_sector() {
	int8_t err;
	char b[24];
	os_snprintf(b, 24, "GET 0x%06X:0x%06X;\n\0", 
			fs.chunk_index * FOTA_CHUNK_SIZE, FOTA_CHUNK_SIZE); 
	
	espconn_send(fs.tcpconn, b, 23); 
	if (err != ESPCONN_OK) {
		ERROR("TCP SEND ERROR: %d\r\n", err);
		_fota_proto_delete();
	}
}


void
_fota_proto_recv_cb(void *arg, char *pdata, unsigned short len) {
	if (len < 3) {
		return;
	}
	uint16_t clen;
	os_memcpy(&clen, pdata, 2);

	char *cursor = fs.recv_buffer + 
		(fs.chunk_index % FOTA_CHUNKS_PER_SECTOR) * FOTA_CHUNK_SIZE;

	os_memcpy(cursor, pdata+2, clen); 
	fs.chunk_index++;
	uint8_t ss = fs.chunk_index % FOTA_CHUNKS_PER_SECTOR;
	if (ss == 0 || clen < FOTA_CHUNK_SIZE) { 
		_fota_write_sector();
		fs.sector++;
		if (clen < FOTA_CHUNK_SIZE) {
			INFO("FOTA: Last chunk\r\n");
			fs.ok = true;
			espconn_disconnect(fs.tcpconn);
			return;
		}
	}
	_fota_get_sector();
}


//void
//_fota_proto_sent_cb(void *arg) {
//	SpiFlashOpResult err;
//	uint8_t ss = (fs.chunk_index+1) % FOTA_CHUNKS_PER_SECTOR;
//	if (ss != 0) { 
//		return;
//	}
//	ERROR("Ereasing sector: 0x%2X\r\n", fs.sector);
//	err = spi_flash_erase_sector(fs.sector);
//	if (err != SPI_FLASH_RESULT_OK) {
//		ERROR("Cannot erase flash: %d\r\n", err);
//		return;
//	}
//}



void
_fota_proto_disconnect_cb(void *arg) {
	_fota_proto_delete();
	INFO("FOTA: Successfully disconnected\r\n");
	if (!fs.ok) {
		INFO("FOTA Failed\r\n");
		return;
	}
	INFO("FOTA: finish\r\n");
	system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
	INFO("REBOOTING\r\n");
	system_upgrade_reboot();
}


LOCAL void
_fota_proto_connect_cb(void *arg) {
	espconn_regist_recvcb(fs.tcpconn, _fota_proto_recv_cb);
	//espconn_regist_sentcb(fs.tcpconn, _fota_proto_sent_cb);
	espconn_regist_disconcb(fs.tcpconn, _fota_proto_disconnect_cb);
	INFO("FOTA: Connected\r\n");
	system_upgrade_flag_set(UPGRADE_FLAG_START);
	fs.chunk_index = 0;
	fs.recv_buffer = (uint8_t*)os_zalloc(FOTA_SECTOR_SIZE);
	_fota_get_sector();
}


void 
_fota_proto_reconn_cb(void *arg, sint8 errType) {
	INFO("Connection error\r\n");
    system_os_post(1, 0, 0);
}


LOCAL void ICACHE_FLASH_ATTR
_fota_dns_found(const char *name, ip_addr_t *ipaddr, void *arg) {
    struct espconn *tcpconn = (struct espconn *)arg;
    if (ipaddr == NULL)
    {
        ERROR("DNS: Found, but got no ip, try to reconnect\r\n");
		_fota_proto_delete();
		ERROR("ERROR: Cannot resolve DNS\r\n");
        return;
    }

    INFO("DNS: found ip %d.%d.%d.%d\n",
         *((uint8 *) &ipaddr->addr),
         *((uint8 *) &ipaddr->addr + 1),
         *((uint8 *) &ipaddr->addr + 2),
         *((uint8 *) &ipaddr->addr + 3));

    if (fs.ip.addr == 0 && ipaddr->addr != 0)
    {
        os_memcpy(fs.tcpconn->proto.tcp->remote_ip, &ipaddr->addr, 4);
        espconn_connect(tcpconn);
    }
}


void
_fota_task_cb(os_event_t *e) {
    if (UTILS_StrToIP(fs.server, &fs.tcpconn->proto.tcp->remote_ip)) {
        espconn_connect(fs.tcpconn);
    }
    else {
        espconn_gethostbyname(fs.tcpconn, fs.server, &fs.ip, _fota_dns_found);
    }
}

void ICACHE_FLASH_ATTR
fota_start() {
    system_os_post(1, 0, 0);
}


void ICACHE_FLASH_ATTR
fota_init(const char* server, uint16_t server_len) {
	char *colon = strrchr(server, ':');;
	uint8_t hostname_len = (uint8_t)(colon - server);
	uint16_t port = atoi(colon+1);
	colon[0] = 0;	
	fs.sector = system_upgrade_userbin_check() == UPGRADE_FW_BIN1 ?
		SYSTEM_PARTITION_OTA2_ADDR / FOTA_SECTOR_SIZE: 0x1;

	//system_soft_wdt_stop();
	//wifi_fpm_close();
	bool fp = spi_flash_erase_protect_disable();
	if (!fp) {
		INFO("Cannot disable the flash protection\r\n");
		return;
	}
	INFO("Flash protect disabled\r\n");

	INFO("FOTA: Start: %s:%d Sector: %X\r\n", server, port, fs.sector);
	fs.server = (char *)os_zalloc(server_len + 1);
	fs.ok = false;
	os_strcpy(fs.server, server);
	fs.server[server_len] = 0;

    fs.tcpconn = (struct espconn *)os_zalloc(sizeof(struct espconn));
    fs.tcpconn->type = ESPCONN_TCP;
    fs.tcpconn->state = ESPCONN_NONE;
    fs.tcpconn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    fs.tcpconn->proto.tcp->local_port = espconn_port();
    fs.tcpconn->proto.tcp->remote_port = port;
    fs.tcpconn->reverse = &fs; 
    espconn_regist_connectcb(fs.tcpconn, _fota_proto_connect_cb);
    espconn_regist_reconcb(fs.tcpconn, _fota_proto_reconn_cb);

    system_os_task(_fota_task_cb, 1, fota_task_queue, 1);
}

