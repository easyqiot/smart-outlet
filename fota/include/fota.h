
#ifndef FOTA_H_
#define FOTA_H_

#include <os_type.h>
#include <ip_addr.h>
#include <espconn.h>


#ifndef FOTA_TASK_QUEUE_SIZE
#define FOTA_TASK_QUEUE_SIZE	1
#endif


#ifndef FOTA_TASK_PRIO
#define FOTA_TASK_PRIO			1
#endif


#ifndef FOTA_RECONNECT_INTERVAL
#define FOTA_RECONNECT_INTERVAL		2000
#endif


#ifndef FOTA_SECTOR_SIZE
#define FOTA_SECTOR_SIZE	4096
#endif


#ifndef FOTA_CHUNK_SIZE
#define FOTA_CHUNK_SIZE		1024	
#endif


#ifndef FOTA_PARTITION_OTA2_ADDR
#define FOTA_PARTITION_OTA2_ADDR		0x81000	
#endif


#define FOTA_CHUNKS_PER_SECTOR	4


enum fota_signal{
	FOTA_SIG_CONNECT,
	FOTA_SIG_RECONNECT,
	FOTA_SIG_DISCONNECT,
	FOTA_SIG_WRITE_SECTOR,
	FOTA_SIG_GET,
	FOTA_SIG_DELETE,
	
	// Callbacks
	FOTA_SIG_CONNECTED,
	FOTA_SIG_DISCONNECTED,
};


enum fota_status {
	FOTA_IDLE,				// 0
	FOTA_CONNECTING,		// 1
	FOTA_DISCONNECTING,		// 2
	FOTA_RECONNECTING,		// 3
	FOTA_CONNECTED,			// 4 TODO: Rename it to READY
};


struct fota_session {
	struct espconn *tcpconn;
	char *hostname;
	uint16_t port;
	enum fota_status status;
	ip_addr_t ip;
	ETSTimer timer; // TODO: rename it to reconnect_timer
	uint64_t reconnect_ticks;
	char * recv_buffer;
	size_t recvbuffer_length;
	uint32_t sector;
	uint16_t chunk_index;
	bool ok;
};


void ICACHE_FLASH_ATTR 
fota_start(); 

void ICACHE_FLASH_ATTR 
fota_init(const char *hostname, uint8_t hostname_len, uint16_t port);

#endif

