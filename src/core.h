#ifndef CORE_H
#define CORE_H

#include <Python.h>
#include "dht/dht.h"

typedef struct {
	int s, s6, port;
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

#endif /* CORE_H */

