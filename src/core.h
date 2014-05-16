#ifndef CORE_H
#define CORE_H

#include <Python.h>

#define CHECK_DHT(self)                                        \
	if ((self)->dht == NULL) {                                   \
		PyErr_SetString(DHTError, "jcdht object killed.");     \
		return NULL;                                               \
	}
  
typedef struct {
	int s, s6;
	int have_id;
	unsigned char myid[20];
	time_t tosleep;
	int ipv4, ipv6;
} DHT;

typedef struct {
	PyObject_HEAD
	DHT* dht;
} JCDHT;

void JCDHT_install_dict(void);

enum {
	DHT_ENABLE_IPV4 = 1,
	DHT_ENABLE_IPV6 = 2
};

#endif /* CORE_H */

