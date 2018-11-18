#include <mem.h>
#include <osapi.h>  
#include <ip_addr.h>
#include <c_types.h>
#include <espconn.h>
#include <string.h>
#include <spi_flash.h>
#include <upgrade.h>

#include "fota.h"
#include "debug.h"


LOCAL struct fota_session fs;

// FIXME: May be static
os_event_t fota_task_queue[FOTA_TASK_QUEUE_SIZE];


void
_fota_task_post(enum fota_signal signal) {
    system_os_post(FOTA_TASK_PRIO, signal, NULL);
}


LOCAL void 
_fota_erase_sector() {
	INFO("E: 0x%05X\r\n", fs.sector * FOTA_SECTOR_SIZE);
	SpiFlashOpResult err;
	
	err = spi_flash_erase_sector(fs.sector);
	if (err != SPI_FLASH_RESULT_OK) {
		ERROR("Canot erase flash: %d\r\n", err);
	}
}


LOCAL void 
_fota_write_sector() {
	INFO("W: 0x%05X\r\n", fs.sector * FOTA_SECTOR_SIZE);
	SpiFlashOpResult err;
	
	err = spi_flash_write(fs.sector * FOTA_SECTOR_SIZE, 
			(uint32_t *)&fs.recv_buffer[0], 
			FOTA_SECTOR_SIZE);
	if (err != SPI_FLASH_RESULT_OK) {
		ERROR("Canot write flash: %d\r\n", err);
		return;
	}

}



void
_fota_tcpclient_recv_cb(void *arg, char *pdata, unsigned short len) {
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
		if (clen < FOTA_CHUNK_SIZE) {
			INFO("FOTA: Last chunk\r\n");
			fs.ok = true;
		}
		_fota_task_post(FOTA_SIG_ERASE_SECTOR);
		return;
	}
	_fota_task_post(FOTA_SIG_GET);
}


void 
_fota_tcpclient_connection_error_cb(void *arg, sint8 errType) {
	INFO("Connection error\r\n");
	_fota_task_post(FOTA_SIG_RECONNECT);
}


void
_fota_tcpclient_disconnect_cb(void *arg) {
	_fota_task_post(FOTA_SIG_DISCONNECTED);
}


void
_fota_tcpclient_connect_cb(void *arg) {
	if (fs.status == FOTA_CONNECTING) {
		_fota_task_post(FOTA_SIG_CONNECTED);
	}
}


void
_fota_proto_delete() {
    if (fs.tcpconn != NULL) {
        espconn_delete(fs.tcpconn);
        if (fs.tcpconn->proto.tcp)
            os_free(fs.tcpconn->proto.tcp);
        os_free(fs.tcpconn);
        fs.tcpconn = NULL;
    }
}


LOCAL void
_fota_proto_connect() {
    fs.tcpconn = (struct espconn *)os_zalloc(sizeof(struct espconn));
    fs.tcpconn->type = ESPCONN_TCP;
    fs.tcpconn->state = ESPCONN_NONE;
    fs.tcpconn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    fs.tcpconn->proto.tcp->local_port = espconn_port();
    fs.tcpconn->proto.tcp->remote_port = fs.port;
    espconn_regist_connectcb(fs.tcpconn, _fota_tcpclient_connect_cb);
    espconn_regist_reconcb(fs.tcpconn, _fota_tcpclient_connection_error_cb);

    if (UTILS_StrToIP(fs.hostname, &fs.tcpconn->proto.tcp->remote_ip)) {
        espconn_connect(fs.tcpconn);
    }
    else {
		ERROR("Invalid IP: %s\r\n", fs.hostname);
    }
}


LOCAL void
_fota_proto_get_sector() {
	int8_t err;
	char b[24];
	os_snprintf(b, 24, "GET 0x%06X:0x%06X;\n\0", 
			fs.chunk_index * FOTA_CHUNK_SIZE, FOTA_CHUNK_SIZE); 
	espconn_send(fs.tcpconn, b, 23); 
}



// TODO: Rename it to reconnect timer
// TODO: LOCAL/static
void 
_fota_timer_cb(void *arg) {
	fs.reconnect_ticks++;
	INFO("Status: %d, Ticks: %d\r\n", fs.status, fs.reconnect_ticks);
	if (fs.status == FOTA_RECONNECTING) {
		INFO("Reconnecting\r\n");
		_fota_proto_delete();
		_fota_task_post(FOTA_SIG_CONNECT);
	}
}


void 
_fota_task_cb(os_event_t *e)
{
    switch (e->sig) {
    case FOTA_SIG_CONNECT:
		fs.status = FOTA_CONNECTING;
		_fota_proto_connect();
        break;

    case FOTA_SIG_CONNECTED:
	    os_timer_disarm(&fs.timer);
		fs.status = FOTA_CONNECTED;
		espconn_regist_recvcb(fs.tcpconn, _fota_tcpclient_recv_cb);
		espconn_regist_disconcb(fs.tcpconn, _fota_tcpclient_disconnect_cb);
		INFO("FOTA: Connected\r\n");
		system_upgrade_flag_set(UPGRADE_FLAG_START);
		_fota_task_post(FOTA_SIG_GET);
		break;
	
	case FOTA_SIG_GET:
		_fota_proto_get_sector();
		break;

	case FOTA_SIG_ERASE_SECTOR:
		_fota_erase_sector();
		_fota_task_post(FOTA_SIG_WRITE_SECTOR);
		break;

	case FOTA_SIG_WRITE_SECTOR:
		_fota_write_sector();
		if (fs.ok) {
			// Last one
			_fota_task_post(FOTA_SIG_DISCONNECT);
		}
		else {
			os_memset(fs.recv_buffer, 0, FOTA_SECTOR_SIZE);
			fs.sector++;
			_fota_task_post(FOTA_SIG_GET);
		}
		break;

    case FOTA_SIG_RECONNECT:
		fs.status = FOTA_RECONNECTING;

	    os_timer_disarm(&fs.timer);
	    os_timer_setfn(&fs.timer, (os_timer_func_t *)_fota_timer_cb, NULL);
	    os_timer_arm(&fs.timer, FOTA_RECONNECT_INTERVAL, 0);
		break;

	case FOTA_SIG_DISCONNECT:
		fs.status = FOTA_DISCONNECTING;
		espconn_disconnect(fs.tcpconn);
		break;

	case FOTA_SIG_DISCONNECTED:
	    fs.status = FOTA_IDLE;
		INFO("FOTA: Successfully disconnected\r\n");
		_fota_proto_delete();
		if (!fs.ok) {
			INFO("FOTA Failed\r\n");
			return;
		}
		INFO("FOTA: finish\r\n");
		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
		INFO("REBOOTING\r\n");
		system_upgrade_reboot();
		break;

	case FOTA_SIG_DELETE:
		_fota_proto_delete();
    	os_free(fs.hostname);
    	os_free(fs.recv_buffer);
		break;
	}
}


/* Schedules a connect request 
 */
void ICACHE_FLASH_ATTR 
fota_start() {
	if (fs.status != FOTA_IDLE) {
		return;
	}
	INFO("FOTA: Start: %s:%d Sector: %X\r\n", fs.hostname, fs.port, fs.sector);
	return _fota_task_post(FOTA_SIG_CONNECT);
}


/* Alocate memory and inititalize the task
 */
void ICACHE_FLASH_ATTR 
fota_init(const char *hostname, uint8_t hostname_len, uint16_t port) {
	fs.hostname = (char *)os_zalloc(hostname_len + 1);
	os_strncpy(fs.hostname, hostname, hostname_len);

	fs.recv_buffer = (char*)os_zalloc(FOTA_SECTOR_SIZE);

	fs.port = port;
	fs.reconnect_ticks = 0;
	fs.status = FOTA_IDLE;
	fs.sector = system_upgrade_userbin_check() == UPGRADE_FW_BIN1 ?
		FOTA_PARTITION_OTA2_ADDR / FOTA_SECTOR_SIZE: 1;

	fs.chunk_index = 0;
	fs.ok = false;
	//system_soft_wdt_stop();
	wifi_fpm_close();

	bool fp = spi_flash_erase_protect_disable();
	if (!fp) {
		INFO("Cannot disable the flash protection\r\n");
		return;
	}

	INFO("FOTA: Init %s:%d Sector: %X\r\n", fs.hostname, fs.port, fs.sector);
    system_os_task(_fota_task_cb, FOTA_TASK_PRIO, fota_task_queue, 
			FOTA_TASK_QUEUE_SIZE);
}



