
#include <mem.h>
#include <c_types.h>
#include <spi_flash.h>
#include <upgrade.h>

#include "debug.h"


// HEADER
#include <ip_addr.h>
#include <espconn.h>

typedef enum {
	FOTA_IDLE,				// 0
	FOTA_CONNECTING,		// 1
	FOTA_DISCONNECTING,		// 2
	FOTA_DOWNLOADING,		// 4
	FOTA_CONNECTED,			// 5 TODO: Rename it to READY
} FOTAStatus;


struct fota_session {
	struct espconn *tcpconn;
	ip_addr_t ip;
	uint16_t port;
	char *login;
	FOTAStatus status;
	char * recv_buffer;
	size_t recvbuffer_length;
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
}


void
_fota_proto_recv_cb(void *arg, char *pdata, unsigned short len) {
    struct espconn *tcpconn = (struct espconn *)arg;
}


void
_fota_proto_sent_cb(void *arg) {
    struct espconn *tcpconn = (struct espconn *)arg;
}


void 
_fota_proto_reconn_cb(void *arg, sint8 errType) {
    struct espconn *tcpconn = (struct espconn *)arg;
	INFO("Connection error\r\n");
	_fota_proto_delete();
}


void
_fota_proto_disconnect_cb(void *arg) {
    struct espconn *tcpconn = (struct espconn *)arg;
	_fota_proto_delete();
	// TODO: FINISH and reboot
}


LOCAL void
_fota_proto_connect_cb(void *arg) {
    struct espconn *tcpconn = (struct espconn *)arg;
	espconn_regist_recvcb(tcpconn, _fota_proto_recv_cb);
	espconn_regist_sentcb(tcpconn, _fota_proto_sent_cb);
	espconn_regist_disconcb(tcpconn, _fota_proto_disconnect_cb);
	
	INFO("FOTA: Connected");
	//system_upgrade_flag_set(UPGRADE_FLAG_START);
	// TODO: Deallocate
	//fs.recv_buffer = (uint8_t*)os_malloc(FOTA_RECV_BUFFER_SIZE);
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


void ICACHE_FLASH_ATTR
update_firmware(const char* server, uint16_t server_len) {
	char *colon = strrchr(server, ':');;
	uint8_t hostname_len = (uint8_t)(colon - server);
	uint16_t port = atoi(colon+1);
	colon[0] = 0;	
	INFO("FOTA: Start: %s:%d\r\n", server, port);

    fs.tcpconn = (struct espconn *)os_zalloc(sizeof(struct espconn));
    fs.tcpconn->type = ESPCONN_TCP;
    fs.tcpconn->state = ESPCONN_NONE;
    fs.tcpconn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    fs.tcpconn->proto.tcp->local_port = espconn_port();
    fs.tcpconn->proto.tcp->remote_port = port;
    fs.tcpconn->reverse = &fs; 
    espconn_regist_connectcb(fs.tcpconn, _fota_proto_connect_cb);
    espconn_regist_reconcb(fs.tcpconn, _fota_proto_reconn_cb);

    if (UTILS_StrToIP(server, &fs.tcpconn->proto.tcp->remote_ip)) {
        espconn_connect(fs.tcpconn);
    }
    else {
        espconn_gethostbyname(fs.tcpconn, server, &fs.ip, _fota_dns_found);
    }

}


//#define USER2_ADDRESS			0x81
//#define FOTA_CHUNK_SIZE			512
//#define FOTA_CHUNK_PER_SECTOR	8
//#define SECTOR_SIZE				4096


//LOCAL void printbinary(unsigned char *data, uint16_t len) {
//	int i;
//	for (i = 0; i < 8; i++) {
//		INFO(".%02X", data[i]);
//	}
//
//	for (i = len-9; i < len; i++) {
//		INFO(".%02X", data[i]);
//	}
//	INFO("\r\n");
//}


//LOCAL void write_sector(uint32_t n) {
//	spi_flash_erase_sector(n);
//	spi_flash_write(n * SECTOR_SIZE, (uint32_t*)&sector[0], SECTOR_SIZE);
//	os_memset(&sector[0], 0, SECTOR_SIZE);
//}
//
//
//void ICACHE_FLASH_ATTR
//update_firmware(const char* msg, uint16_t message_len) {
//	if (msg[0] == 'R') {
//		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
//		INFO("REBOOTING\r\n");
//		system_upgrade_reboot();
//		return;
//	}
//	if (msg[0] == 'S') {
//		fota_chunks = 0;
//		os_memset(&sector, 0, SECTOR_SIZE);
//		system_upgrade_flag_set(UPGRADE_FLAG_START);
//		INFO("FOTA: Start\r\n");
//		return;
//	}
//
//	size_t olen;
//	FotaChunk chunk;
//	uint8_t sectornum = fota_chunks / FOTA_CHUNK_PER_SECTOR;
//	uint8_t subsector = fota_chunks % FOTA_CHUNK_PER_SECTOR;
//	uint32_t sector_addr = USER2_ADDRESS + sectornum;
//	if (msg[0] == 'D') {
//		os_memset(&chunk, 0, FOTA_CHUNK_SIZE+2);
//		easyq_base64_decode((char*)&chunk, FOTA_CHUNK_SIZE+2, &olen, msg+1, 
//				message_len-1);
//		unsigned char *ad = &sector[0] + FOTA_CHUNK_SIZE * subsector;
//		os_memcpy(ad, &chunk.data, chunk.len);
//		if (subsector == 7) {
//			write_sector(sector_addr);
//			INFO("FOTA: %d:%X\r\n", sectornum, sector_addr);
//		}
//		fota_chunks++;
//	} 
//	else if (msg[0] == 'F') {
//		if (subsector > 0) {
//			INFO("FOTA: Writing remaining bytes at: %X\r\n", sector_addr);
//			write_sector(sector_addr);
//		}
//		fota_chunks = 0;
//		INFO("FOTA: finish\r\n");
//		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
//		INFO("REBOOTING\r\n");
//		system_upgrade_reboot();
//	}
//}

