/* This example code was written by Juliusz Chroboczek.
   You are free to cut'n'paste from it to your heart's content. */

/* For crypt */
#define _GNU_SOURCE

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

#ifdef ENABLE_OPENSSL
#include <openssl/sha.h>
#include <openssl/rand.h>
#endif

#include "core.h"

PyObject* DHTError;

static PyObject* JCDHT_callback_stub(JCDHT* self, PyObject* args)
{
	Py_RETURN_NONE;
}

int main(int argc, char **argv)
{
	int i, rc, fd;
	int s = -1, s6 = -1, port;
	int have_id = 0;
	unsigned char myid[20];
	time_t tosleep = 0;
	char *id_file = "dht-example.id";
	int opt;
	int quiet = 0, ipv4 = 1, ipv6 = 1;
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	struct sockaddr_storage from;
	socklen_t fromlen;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;

	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;



	while(1)
	{
		opt = getopt(argc, argv, "q46b:i:");
		if(opt < 0)
			break;

		switch(opt)
		{
		case 'q':
			quiet = 1;
			break;
		case '4':
			ipv6 = 0;
			break;
		case '6':
			ipv4 = 0;
			break;
		case 'b':
		{
			char buf[16];
			int rc;
			rc = inet_pton(AF_INET, optarg, buf);
			if(rc == 1)
			{
				memcpy(&sin.sin_addr, buf, 4);
				break;
			}
			rc = inet_pton(AF_INET6, optarg, buf);
			if(rc == 1)
			{
				memcpy(&sin6.sin6_addr, buf, 16);
				break;
			}
			goto usage;
		}
		break;
		case 'i':
			id_file = optarg;
			break;
		default:
			goto usage;
		}
	}

	/* Ids need to be distributed evenly, so you cannot just use your
	   bittorrent id.  Either generate it randomly, or take the SHA-1 of
	   something. */
	fd = open(id_file, O_RDONLY);
	if(fd >= 0)
	{
		rc = read(fd, myid, 20);
		if(rc == 20)
			have_id = 1;
		close(fd);
	}

	fd = open("/dev/urandom", O_RDONLY);
	if(fd < 0)
	{
		perror("open(random)");
		exit(1);
	}

	if(!have_id)
	{
		int ofd;

		rc = read(fd, myid, 20);
		if(rc < 0)
		{
			perror("read(random)");
			exit(1);
		}
		have_id = 1;
		close(fd);

		ofd = open(id_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if(ofd >= 0)
		{
			rc = write(ofd, myid, 20);
			if(rc < 20)
				unlink(id_file);
			close(ofd);
		}
	}

	{
		unsigned seed;
		read(fd, &seed, sizeof(seed));
		srandom(seed);
	}

	close(fd);

	if(argc < 2)
		goto usage;

	i = optind;

	if(argc < i + 1)
		goto usage;

	port = atoi(argv[i++]);
	if(port <= 0 || port >= 0x10000)
		goto usage;

	while(i < argc)
	{
		struct addrinfo hints, *info, *infop;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_DGRAM;
		if(!ipv6)
			hints.ai_family = AF_INET;
		else if(!ipv4)
			hints.ai_family = AF_INET6;
		else
			hints.ai_family = 0;
		rc = getaddrinfo(argv[i], argv[i + 1], &hints, &info);
		if(rc != 0)
		{
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
			exit(1);
		}

		i++;
		if(i >= argc)
			goto usage;

		infop = info;
		while(infop)
		{
			memcpy(&bootstrap_nodes[num_bootstrap_nodes],
			       infop->ai_addr, infop->ai_addrlen);
			infop = infop->ai_next;
			num_bootstrap_nodes++;
		}
		freeaddrinfo(info);

		i++;
	}

	/* If you set dht_debug to a stream, every action taken by the DHT will
	   be logged. */
	if(!quiet)
		dht_debug = stdout;

	/* We need an IPv4 and an IPv6 socket, bound to a stable port.  Rumour
	   has it that uTorrent works better when it is the same as your
	   Bittorrent port. */
	if(ipv4)
	{
		s = socket(PF_INET, SOCK_DGRAM, 0);
		if(s < 0)
		{
			perror("socket(IPv4)");
		}
	}

	if(ipv6)
	{
		s6 = socket(PF_INET6, SOCK_DGRAM, 0);
		if(s6 < 0)
		{
			perror("socket(IPv6)");
		}
	}

	if(s < 0 && s6 < 0)
	{
		fprintf(stderr, "Eek!");
		exit(1);
	}


	if(s >= 0)
	{
		sin.sin_port = htons(port);
		rc = bind(s, (struct sockaddr*)&sin, sizeof(sin));
		if(rc < 0)
		{
			perror("bind(IPv4)");
			exit(1);
		}
	}

	if(s6 >= 0)
	{
		int rc;
		int val = 1;

		rc = setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY,
		                (char *)&val, sizeof(val));
		if(rc < 0)
		{
			perror("setsockopt(IPV6_V6ONLY)");
			exit(1);
		}

		/* BEP-32 mandates that we should bind this socket to one of our
		   global IPv6 addresses.  In this simple example, this only
		   happens if the user used the -b flag. */

		sin6.sin6_port = htons(port);
		rc = bind(s6, (struct sockaddr*)&sin6, sizeof(sin6));
		if(rc < 0)
		{
			perror("bind(IPv6)");
			exit(1);
		}
	}

	/* Init the dht.  This sets the socket into non-blocking mode. */
	rc = dht_init(s, s6, myid, (unsigned char*)"JC\0\0");
	if(rc < 0)
	{
		perror("dht_init");
		exit(1);
	}

	/* For bootstrapping, we need an initial list of nodes.  This could be
	   hard-wired, but can also be obtained from the nodes key of a torrent
	   file, or from the PORT bittorrent message.

	   Dht_ping_node is the brutal way of bootstrapping -- it actually
	   sends a message to the peer.  If you're going to bootstrap from
	   a massive number of nodes (for example because you're restoring from
	   a dump) and you already know their ids, it's better to use
	   dht_insert_node.  If the ids are incorrect, the DHT will recover. */
	for(i = 0; i < num_bootstrap_nodes; i++)
	{
		dht_ping_node((struct sockaddr*)&bootstrap_nodes[i],
		              sizeof(bootstrap_nodes[i]));
		usleep(random() % 100000);
	}

	/*while(1) do stuff */

	{
		struct sockaddr_in sin[500];
		struct sockaddr_in6 sin6[500];
		int num = 500, num6 = 500;
		int i;
		i = dht_get_nodes(sin, &num, sin6, &num6);
		printf("Found %d (%d + %d) good nodes.\n", i, num, num6);
	}

	dht_uninit();
	return 0;

usage:
	printf("Usage: dht-example [-q] [-4] [-6] [-i filename] [-b address]...\n"
	       "                   port [address port]...\n");
	exit(1);
}

static PyObject* JCDHT_do(JCDHT *self, PyObject* args)
{
	CHECK_DHT(self);
	
	struct timeval tv;
	fd_set readfds;
	unsigned char buf[4096];
	int s = self->dht->s;
	int s6 = self->dht->s6;
	int port = self->dht->port;
	int have_id = self->dht->have_id;
	unsigned char *myid = self->dht->myid;
	int ipv4 = self->dht->ipv4;
	int ipv6 = self->dht->ipv6;
	
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
		                  &self->dht->tosleep, callback_search, NULL); //TODO: replace NULL with object
	}
	else
	{
		rc = dht_periodic(NULL, 0, NULL, 0, &self->dht->tosleep, callback_search, NULL); //TODO: replace NULL with object
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

	/* This is how you trigger a search for a torrent hash.  If port
	   (the second argument) is non-zero, it also performs an announce.
	   Since peers expire announced data after 30 minutes, it's a good
	   idea to reannounce every 28 minutes or so. */
	/*if(searching)
	{
		if(s >= 0)
			dht_search(hash, 0, AF_INET, callback_search, NULL); //TODO: replace NULL with object
		if(s6 >= 0)
			dht_search(hash, 0, AF_INET6, callback_search, NULL); //TODO: replace NULL with object
		searching = 0;
	}*/

	if (PyErr_Occurred())
	{
		return NULL;
	}
	
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
void
dht_hash(void *hash_return, int hash_size,
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
void
dht_hash(void *hash_return, int hash_size,
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

int
dht_random_bytes(void *buf, size_t size)
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

static void callback_search(void *self, int event, unsigned char *info_hash,
                                                 void *data, size_t data_len)
{
	CHECK_DHT(self);
	
#if PY_MAJOR_VERSION < 3
	PyObject_CallMethod((PyObject*)self, "on_search", "issK", event, info_hash, data, data_len); //TODO: fix format
#else
	PyObject_CallMethod((PyObject*)self, "on_search", "iyyK", event, info_hash, data, data_len); //TODO: fix format
#endif
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

static int JCDHT_dealloc(JCDHT* self)
{
	if (self->tox)
	{
		dht_uninit();
		free(self->dht);
		self->dht = NULL;
	}
	return 0;
}

static int init_helper(JCDHT* self, PyObject* args)
{
	if(!self->dht)
	{
		unsigned seed;
		JCDHT *dht = malloc(sizeof(JCDHT));
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
		char *id;
		int idlen, port, sockflags = 3;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
		JCDHT *dht = self->dht;
		
		/* the address that we will bind to */
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		
#if PY_MAJOR_VERSION < 3
		if(!PyArg_ParseTuple(args, "s#i|i", id, &idlen, &port, &sockflags))
#else
		if(!PyArg_ParseTuple(args, "y#i|i", id, &idlen, &port, &sockflags))
#endif
		{
			PyErr_SetString(PyExcValueError, "Failed to parse arguments");
			return -1;
		}

		if(idlen != 20)
		{
			PyErr_SetString(PyExcValueError, "ID must be 20 bytes, generated randomly and then should be saved/reused");
			return -1;
		}
		memcpy(&dht->myid, id, 20);
		self->have_id = 1;
		
		dht->ipv4 = (DHT_ENABLE_IPV4 & sockflags) == DHT_ENABLE_IPV4;
		dht->ipv6 = (DHT_ENABLE_IPV6 & sockflags) == DHT_ENABLE_IPV6;
		if(ipv4 == 0 && ipv6 == 0)
		{
			PyErr_SetString(PyExcValueError, "At least one network stack must be enabled");
			return -1;
		}
		
		if(port <= 0 || port >= 0x10000)
		{
			PyErr_SetString(PyExcValueError, "Wrong port value");
			return -1;
		}
		dht->port = port; //TODO: is dht->port actually needed after binding?

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
		
		if(s >= 0)
		{
			sin.sin_port = htons(port);
			rc = bind(dht->s, (struct sockaddr*)&sin, sizeof(sin));
			if(rc < 0)
			{
				PyErr_SetString(PyExc_IOError, "Error binding IPv4 socket");
				return -1;
			}
		}

		if(s6 >= 0)
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
		
		//TODO: is there something left?
	}
}

PyMethodDef DHT_methods[] =
{
	{
		"on_search", (PyCFunction)JCDHT_callback_stub, METH_VARARGS,
		"on_search(event, info_hash, data, data_len)\n"
		"Callback called when receiving peers or search done, "
		"default implementation does nothing."
	},
	{
		"do", (PyCFunction)JCDHT_do, METH_NOARGS,
		"do()\n"
		"The main loop."
	}
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
	"JCDHT object",          /* tp_doc */
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
	(initproc)JCDHT_init,    /* tp_init */
	0,                         /* tp_alloc */
	JCDHT_new,               /* tp_new */
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
	SET(ENABLE_IPV4)
	SET(ENABLE_IPV6)

#undef SET

	ToxCoreType.tp_dict = dict;
}
