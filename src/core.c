/* This example code was written by Juliusz Chroboczek.
   You are free to cut'n'paste from it to your heart's content. */

/* For crypt */
#if defined(_GNU_SOURCE) && !defined(ENABLE_OPENSSL)
#define _GNU_SOURCE
#endif

#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/signal.h>
#include <assert.h>

#ifdef ENABLE_OPENSSL
#include <openssl/sha.h>
#include <openssl/rand.h>
#endif

#include "core.h"
#include "dht/dht.h"

PyObject* DHTError;

//TODO: better error checking for inet_ntop

/* For bootstrapping, we need an initial list of nodes.  This could be
   hard-wired, but can also be obtained from the nodes key of a torrent
   file, or from the PORT bittorrent message.

   Dht_ping_node is the brutal way of bootstrapping -- it actually
   sends a message to the peer.  If you're going to bootstrap from
   a massive number of nodes (for example because you're restoring from
   a dump) and you already know their ids, it's better to use
   dht_insert_node.  If the ids are incorrect, the DHT will recover. */

/*{
	struct sockaddr_in sin[500];
	struct sockaddr_in6 sin6[500];
	int num = 500, num6 = 500;
	int i;
	i = dht_get_nodes(sin, &num, sin6, &num6);
	printf("Found %d (%d + %d) good nodes.\n", i, num, num6);
}*/


static PyObject* JCDHT_callback_stub(JCDHT* self, PyObject* args)
{
	Py_RETURN_NONE;
}

static void callback_search(void *self, int event, const unsigned char *info_hash,
                                                const void *data, size_t data_len)
{
	if(self == NULL)
	{
		PyErr_SetString(DHTError, "danger to manifold."); 
		return;
	}
	if(((JCDHT*)self)->dht == NULL)
	{
		PyErr_SetString(DHTError, "jcdht object killed."); 
		return;
	}
	
	Py_ssize_t num_results;
	switch(event)
	{
		case DHT_EVENT_VALUES:
			num_results = data_len / 6;
			assert(data_len % 6 == 0);
			break;
		case DHT_EVENT_VALUES6:
			num_results = data_len / 20;
			assert(data_len % 20 == 0);
			break;
		default:
			num_results = 0;
			break;
	}
	
	PyObject *peerlist = PyList_New(num_results);
	char stringbuf[INET6_ADDRSTRLEN];
	const uint8_t * walk = data;
	uint16_t portbuf;
	PyObject *tup;
	int i;
	
	if(event == DHT_EVENT_VALUES)
	{
		for(i=0; i<num_results; i++)
		{
			if(inet_ntop(AF_INET, walk, stringbuf, sizeof(stringbuf)) == NULL)
			{
				perror("inet_ntop");
				return;
			}
			
			walk += 4;
			memcpy(&portbuf, walk, sizeof(portbuf));
			walk += 2;
			
			tup = Py_BuildValue("(si)", stringbuf, ntohs(portbuf));
			PyList_SET_ITEM(peerlist, i, tup);
		}
	}

	if(event == DHT_EVENT_VALUES6)
	{
		for(i=0; i<num_results; i++)
		{
			if(inet_ntop(AF_INET6, walk, stringbuf, sizeof(stringbuf)) == NULL)
			{
				perror("inet_ntop");
				return;
			}
			
			walk += 16;
			memcpy(&portbuf, walk, sizeof(portbuf));
			walk += 2;
			
			tup = Py_BuildValue("(si)", stringbuf, ntohs(portbuf));
			PyList_SET_ITEM(peerlist, i, tup);
		}
	}
	
#if PY_MAJOR_VERSION < 3
	PyObject_CallMethod((PyObject*)self, "on_search", "is#O", event, info_hash, 20, peerlist);
#else
	PyObject_CallMethod((PyObject*)self, "on_search", "iy#O", event, info_hash, 20, peerlist);
#endif
}

static int init_helper(JCDHT* self, PyObject* args)
{
	if(self->dht == NULL)
	{
		unsigned seed;
		DHT *dht = malloc(sizeof(DHT));
		if(!dht)
		{
			PyErr_SetString(DHTError, "dht malloc error");
			return -1;
		}
		
		dht->s = -1;
		dht->s6 = -1;
		dht->have_id = 0;
		dht->tosleep = 0;
		
		dht_random_bytes(&seed, sizeof(seed));
		srandom(seed);
		
		self->dht = dht;
		
		return 0;
	}
	
	if(args) //TODO: add bind to address support
	{
		unsigned char *myid = NULL;
		int rc, idlen, port, sockflags = 3;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
		DHT *dht = self->dht;
		
		/* the address that we will bind to */
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		
#if PY_MAJOR_VERSION < 3
		rc = PyArg_ParseTuple(args, "s#i|i", &myid, &idlen, &port, &sockflags);
#else
		rc = PyArg_ParseTuple(args, "y#i|i", &myid, &idlen, &port, &sockflags);
#endif
		if(!rc)
		{
			PyErr_SetString(PyExc_ValueError, "Failed to parse arguments");
			return -1;
		}

		if(idlen != 20)
		{
			PyErr_SetString(PyExc_ValueError, "ID must be 20 bytes, generated randomly and then should be saved/reused");
			return -1;
		}
		memcpy(&dht->myid, myid, 20);
		dht->have_id = 1;
		
		dht->ipv4 = (DHT_IPV4 & sockflags) == DHT_IPV4;
		dht->ipv6 = (DHT_IPV6 & sockflags) == DHT_IPV6;
		if(dht->ipv4 == 0 && dht->ipv6 == 0)
		{
			PyErr_SetString(PyExc_ValueError, "At least one network stack must be enabled");
			return -1;
		}
		
		if(port <= 0 || port >= 0x10000)
		{
			PyErr_SetString(PyExc_ValueError, "Wrong port value");
			return -1;
		}

#ifdef ENABLE_VERBOSE
		dht_debug = stderr;
#endif

		if(dht->ipv4)
		{
			dht->s = socket(PF_INET, SOCK_DGRAM, 0);
			if(dht->s < 0)
			{
				perror("socket(IPv4)");
			}
		}

		if(dht->ipv6)
		{
			dht->s6 = socket(PF_INET6, SOCK_DGRAM, 0);
			if(dht->s6 < 0)
			{
				perror("socket(IPv6)");
			}
		}

		if(dht->s < 0 && dht->s6 < 0)
		{
			PyErr_SetString(PyExc_IOError, "Error creating sockets");
			return -1;
		}
		
		if(dht->s >= 0)
		{
			sin.sin_port = htons(port);
			rc = bind(dht->s, (struct sockaddr*)&sin, sizeof(sin));
			if(rc < 0)
			{
				PyErr_SetString(PyExc_IOError, "Error binding IPv4 socket");
				return -1;
			}
		}

		if(dht->s6 >= 0)
		{
			int rc;
			int val = 1;

			rc = setsockopt(dht->s6, IPPROTO_IPV6, IPV6_V6ONLY,
							(char *)&val, sizeof(val));
			if(rc < 0)
			{
				PyErr_SetString(PyExc_IOError, "Error setsockopt(IPV6_V6ONLY)");
				return -1;
			}

			/* BEP-32 mandates that we should bind this socket to one of our
			   global IPv6 addresses.  In this simple example, this only
			   happens if the user used the -b flag. */

			sin6.sin6_port = htons(port);
			rc = bind(dht->s6, (struct sockaddr*)&sin6, sizeof(sin6));
			if(rc < 0)
			{
				PyErr_SetString(PyExc_IOError, "Error binding IPv6 socket");
				return -1;
			}
		}

		/* Init the dht.  This sets the socket into non-blocking mode. */
		rc = dht_init(dht->s, dht->s6, dht->myid, NULL);
		if(rc < 0)
		{
			PyErr_SetString(PyExc_RuntimeError, "Error initializing DHT");
			return -1;
		}
	
	}
	
	return 0;
}

static PyObject* JCDHT_do(JCDHT *self, PyObject* args)
{
	CHECK_DHT(self);
	
	struct timeval tv;
	fd_set readfds;
	unsigned char buf[4096];
	int s = self->dht->s;
	int s6 = self->dht->s6;
    struct sockaddr_storage from;
    socklen_t fromlen;
	int rc;
	
	tv.tv_sec = self->dht->tosleep;
	tv.tv_usec = random() % 1000000;

	FD_ZERO(&readfds);
	if(s >= 0)
		FD_SET(s, &readfds);
	if(s6 >= 0)
		FD_SET(s6, &readfds);
	rc = select(s > s6 ? s + 1 : s6 + 1, &readfds, NULL, NULL, &tv);
	if(rc < 0)
	{
		if(errno != EINTR)
		{
			perror("select");
			sleep(1);
		}
	}

	if(rc > 0)
	{
		fromlen = sizeof(from);
		if(s >= 0 && FD_ISSET(s, &readfds))
			rc = recvfrom(s, buf, sizeof(buf) - 1, 0,
			              (struct sockaddr*)&from, &fromlen);
		else if(s6 >= 0 && FD_ISSET(s6, &readfds))
			rc = recvfrom(s6, buf, sizeof(buf) - 1, 0,
			              (struct sockaddr*)&from, &fromlen);
		else
			{
				PyErr_SetString(DHTError, "socket error");
				return NULL;
			}
	}

	if(rc > 0)
	{
		buf[rc] = '\0';
		rc = dht_periodic(buf, rc, (struct sockaddr*)&from, fromlen,
		                  &self->dht->tosleep, callback_search, self); //TODO: replace NULL with object
	}
	else
	{
		rc = dht_periodic(NULL, 0, NULL, 0, &self->dht->tosleep, callback_search, self); //TODO: replace NULL with object
	}
	if(rc < 0)
	{
		if(errno == EINTR)
		{
			Py_RETURN_NONE;
		}
		else
		{
			perror("dht_periodic");
			if(rc == EINVAL || rc == EFAULT)
			{
				PyErr_SetString(DHTError, "failed to run dht_periodic");
				return NULL;
			}
			self->dht->tosleep = 1;
		}
	}

#ifdef ENABLE_VERBOSE
	fflush(stderr);
#endif

	if (PyErr_Occurred())
	{
		return NULL;
	}
	
	Py_RETURN_NONE;
}

static PyObject* JCDHT_ping(JCDHT* self, PyObject* args)
{
	CHECK_DHT(self);
	
	char *addr;
	int port, rc = -1;
	char buf[16];
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	
	if(!PyArg_ParseTuple(args, "si", &addr, &port))
	{
		PyErr_SetString(PyExc_ValueError, "Failed to parse arguments");
		return NULL;
	}
	
	if(inet_pton(AF_INET, addr, buf) == 1)
	{
		memcpy(&sin.sin_addr.s_addr, buf, 4);
		sin.sin_port = htons(port);
		sin.sin_family = AF_INET;
		rc = dht_ping_node ((struct sockaddr*)&sin, sizeof (sin));
	}
	else if(inet_pton(AF_INET6, addr, buf) == 1)
	{
		memcpy(&sin6.sin6_addr.s6_addr, buf, 16);
		sin6.sin6_port = htons(port);
		sin6.sin6_family = AF_INET6;
		rc = dht_ping_node ((struct sockaddr*)&sin6, sizeof (sin6));
	}
	else
	{
		PyErr_SetString(PyExc_ValueError, "Failed to parse address");
		return NULL;
	}
	
	if(rc > 0)
	{
		Py_RETURN_TRUE;
	}
	
	Py_RETURN_FALSE;
}

static PyObject* JCDHT_nodes(JCDHT* self, PyObject* args)
{
	CHECK_DHT(self);
	
	int family, goodn, dubiousn, cachedn, incomingn;
	
	if(!PyArg_ParseTuple(args, "i", &family))
	{
		PyErr_SetString(PyExc_ValueError, "Failed to parse arguments");
		return NULL;
	}

	dht_nodes(family == DHT_IPV6 ? AF_INET6 : AF_INET, &goodn, &dubiousn, &cachedn, &incomingn);
	
	PyObject *tup = Py_BuildValue("(iiii)", goodn, dubiousn, cachedn, incomingn);
	
	return tup;
}

static PyObject* JCDHT_get_nodes(JCDHT* self, PyObject* args)
{
	CHECK_DHT(self);
	
	struct sockaddr_in sin[DHT_GET_NODES_MAX];
	struct sockaddr_in6 sin6[DHT_GET_NODES_MAX];
	int num = DHT_GET_NODES_MAX, num6 = DHT_GET_NODES_MAX;
	char stringbuf[INET6_ADDRSTRLEN];
	uint16_t portbuf;
	PyObject *tup;
	int i;

	dht_get_nodes(sin, &num, sin6, &num6);
	
	PyObject *peerlist = PyList_New(num);
	for(i=0; i<num; i++)
	{
		if(inet_ntop(AF_INET, &sin[i].sin_addr.s_addr, stringbuf, sizeof(stringbuf)) == NULL)
		{
			perror("inet_ntop");
			PyErr_SetString(DHTError, "inet_ntop"); 
			return NULL;
		}		
		memcpy(&portbuf, &sin[i].sin_port, sizeof(portbuf));
		
		tup = Py_BuildValue("(si)", stringbuf, ntohs(portbuf));
		PyList_SET_ITEM(peerlist, i, tup);
	}

	PyObject *peerlist6 = PyList_New(num6);
	for(i=0; i<num6; i++)
	{
		if(inet_ntop(AF_INET, &sin6[i].sin6_addr.s6_addr, stringbuf, sizeof(stringbuf)) == NULL)
		{
			perror("inet_ntop");
			PyErr_SetString(DHTError, "inet_ntop"); 
			return NULL;
		}		
		memcpy(&portbuf, &sin6[i].sin6_port, sizeof(portbuf));
		
		tup = Py_BuildValue("(si)", stringbuf, ntohs(portbuf));
		PyList_SET_ITEM(peerlist6, i, tup);
	}
	
	PyObject *nodes = Py_BuildValue("(OO)", peerlist, peerlist6);
	
	return nodes;
}

static PyObject* JCDHT_dump(JCDHT* self, PyObject* args)
{
	CHECK_DHT(self);
	
#ifdef ENABLE_VERBOSE
	dht_dump_tables(stderr);
#endif

	Py_RETURN_NONE;
}

/* Functions called by the DHT. */

int
dht_blacklisted(const struct sockaddr *sa, int salen)
{
	return 0;
}

/* We need to provide a reasonably strong cryptographic hashing function.
   Here's how we'd do it if we had OpenSSL's SHA1 code. */
#ifdef ENABLE_OPENSSL
void dht_hash(void *hash_return, int hash_size,
         const void *v1, int len1,
         const void *v2, int len2,
         const void *v3, int len3)
{
	SHA_CTX sha;

	SHA1_Init(&sha);
	SHA1_Update(&sha, v1, len1);
	SHA1_Update(&sha, v2, len2);
	SHA1_Update(&sha, v3, len3);

	SHA1_Final(hash_return, &sha);
}
#else
/* But for this example, we might as well use something weaker. */
void dht_hash(void *hash_return, int hash_size,
         const void *v1, int len1,
         const void *v2, int len2,
         const void *v3, int len3)
{
	const char *c1 = v1, *c2 = v2, *c3 = v3;
	char key[9];                /* crypt is limited to 8 characters */
	int i;

	memset(key, 0, 9);
#define CRYPT_HAPPY(c) ((c % 0x60) + 0x20)

	for(i = 0; i < 2 && i < len1; i++)
		key[i] = CRYPT_HAPPY(c1[i]);
	for(i = 0; i < 4 && i < len1; i++)
		key[2 + i] = CRYPT_HAPPY(c2[i]);
	for(i = 0; i < 2 && i < len1; i++)
		key[6 + i] = CRYPT_HAPPY(c3[i]);
	strncpy(hash_return, crypt(key, "jc"), hash_size);
}
#endif

int dht_random_bytes(void *buf, size_t size)
{
	int fd, rc, save;
	
#ifdef ENABLE_OPENSSL
	rc = RAND_pseudo_bytes(buf, size);
	if(rc == 1)
		return size;
#endif
	
	fd = open("/dev/urandom", O_RDONLY);
	if(fd < 0)
		return -1;

	rc = read(fd, buf, size);

	save = errno;
	close(fd);
	errno = save;

	return rc;
}

static PyObject* JCDHT_search(JCDHT* self, PyObject* args)
{
	CHECK_DHT(self);
	
	DHT *dht = self->dht;
	unsigned char *infohash;
	int hashlen, rc, port = 0;
	
#if PY_MAJOR_VERSION < 3
	rc = PyArg_ParseTuple(args, "s#|i", &infohash, &hashlen, &port);
#else
	rc = PyArg_ParseTuple(args, "y#|i", &infohash, &hashlen, &port);
#endif

	if(!rc)
	{
		PyErr_SetString(PyExc_ValueError, "Failed to parse arguments");
		return NULL;
	}
	
	if(hashlen != 20)
	{
		PyErr_SetString(PyExc_ValueError, "ID must be 20 bytes");
		return NULL;
	}
	
	if(port < 0 || port >= 0x10000)
	{
		PyErr_SetString(PyExc_ValueError, "Wrong port value");
		return NULL;
	}

	if(dht->s >= 0)
	{
		rc = dht_search(infohash, port, AF_INET, callback_search, self);
		if(rc == -1)
		{
			Py_RETURN_FALSE;
		}
	}
	if(dht->s6 >= 0)
	{
		rc = dht_search(infohash, port, AF_INET6, callback_search, self);
		if(rc == -1)
		{
			Py_RETURN_FALSE;
		}
	}
	
	Py_RETURN_TRUE;
}

static PyObject* JCDHT_new(PyTypeObject *type, PyObject* args, PyObject* kwds)
{
	JCDHT* self = (JCDHT*)type->tp_alloc(type, 0);
	self->dht = NULL;

	// We don't care about subclass's arguments
	if (init_helper(self, NULL) == -1)
	{
		return NULL;
	}

	return (PyObject*)self;
}

static int JCDHT_init(JCDHT* self, PyObject* args, PyObject* kwds)
{
	// TODO: probably this is wrong
	return init_helper(self, args);
}

static int JCDHT_dealloc(JCDHT* self)
{
	if (self->dht)
	{
		dht_uninit();
		free(self->dht);
		self->dht = NULL;
	}
	return 0;
}

PyMethodDef DHT_methods[] =
{
	{
		"on_search", (PyCFunction)JCDHT_callback_stub, METH_VARARGS,
		"on_search(event, info_hash, peerlist)\n"
		"Callback called when receiving peers or search done,\n"
		"default implementation does nothing."
	},
	{
		"do", (PyCFunction)JCDHT_do, METH_NOARGS,
		"do()\n"
		"The main loop."
	},
	{
		"ping", (PyCFunction)JCDHT_ping, METH_VARARGS,
		"ping(adress, port)\n"
		"This is the main bootstrapping primitive."
		"You pass it an address at which you believe that a DHT node may be living,\n"
		"and a query will be sent.  If a node replies, and if there is space in the routing table,\n"
		"it will be inserted, up to 9 nodes can be inserted for every call of do()."
	},
	{
		"search", (PyCFunction)JCDHT_search, METH_VARARGS,
		"search(infohash, port)\n"
		"Starts a search, up to 1024 searches can be in progress at a given time.\n"
		"Port is optional, if set to something different than 0 it will announce the peer to the network,\n"
		"and the port will represent the TCP socket used by the client.\n"
		"Return false if max number of searches is reached."
	},
	{
		"nodes", (PyCFunction)JCDHT_nodes, METH_VARARGS,
		"nodes(family)\n"
		"Return a tuple with the number of good, dubious, cached, incoming nodes.\n"
		"Family can be either DHT.IPV6 or DHT.IPV4 .\n"
		"Before starting a search it is recommended to wait until good is at least 4,\n"
		"and good + dubious is at least 30."
	},
	{
		"get_nodes", (PyCFunction)JCDHT_get_nodes, METH_NOARGS,
		"get_nodes()\n"
		"Return a tuple like (peerlist, peerlist6)."
	},
	{
		"dump", (PyCFunction)JCDHT_dump, METH_NOARGS,
		"dump()\n"
		"Debug function, if compiled with --enable-verbose prints DHT buckets 1to stderr."
	},
	{NULL}
};

PyTypeObject JCDHTType = {
#if PY_MAJOR_VERSION >= 3
	PyVarObject_HEAD_INIT(NULL, 0)
#else
	PyObject_HEAD_INIT(NULL)
	0,                         /*ob_size*/
#endif
	"DHT",                     /*tp_name*/
	sizeof(JCDHT),           /*tp_basicsize*/
	0,                         /*tp_itemsize*/
	(destructor)JCDHT_dealloc, /*tp_dealloc*/
	0,                         /*tp_print*/
	0,                         /*tp_getattr*/
	0,                         /*tp_setattr*/
	0,                         /*tp_compare*/
	0,                         /*tp_repr*/
	0,                         /*tp_as_number*/
	0,                         /*tp_as_sequence*/
	0,                         /*tp_as_mapping*/
	0,                         /*tp_hash */
	0,                         /*tp_call*/
	0,                         /*tp_str*/
	0,                         /*tp_getattro*/
	0,                         /*tp_setattro*/
	0,                         /*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
	"JCDHT object",            /* tp_doc */
	0,                         /* tp_traverse */
	0,                         /* tp_clear */
	0,                         /* tp_richcompare */
	0,                         /* tp_weaklistoffset */
	0,                         /* tp_iter */
	0,                         /* tp_iternext */
	DHT_methods,               /* tp_methods */
	0,                         /* tp_members */
	0,                         /* tp_getset */
	0,                         /* tp_base */
	0,                         /* tp_dict */
	0,                         /* tp_descr_get */
	0,                         /* tp_descr_set */
	0,                         /* tp_dictoffset */
	(initproc)JCDHT_init,      /* tp_init */
	0,                         /* tp_alloc */
	JCDHT_new,                 /* tp_new */
};

void JCDHT_install_dict()
{
#define SET(name)                                            \
	PyObject* obj_##name = PyLong_FromLong(DHT_##name);        \
	PyDict_SetItemString(dict, #name, obj_##name);             \
	Py_DECREF(obj_##name);

	PyObject* dict = PyDict_New();
	SET(EVENT_NONE)
	SET(EVENT_VALUES)
	SET(EVENT_VALUES6)
	SET(EVENT_SEARCH_DONE)
	SET(EVENT_SEARCH_DONE6)
	SET(IPV4)
	SET(IPV6)

#undef SET

	JCDHTType.tp_dict = dict;
}
