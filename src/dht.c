#include <Python.h>
#include <stdio.h>

#include "core.h"

extern PyObject* DHTError;
extern PyTypeObject JCDHTType;

#if PY_MAJOR_VERSION >= 3
struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"dht",
	"Python JCDHT module",
	-1,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};
#endif

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_dht(void)
{
	PyObject *m = PyModule_Create(&moduledef);
#else
PyMODINIT_FUNC initdht(void)
{
	PyObject *m = Py_InitModule("dht", NULL);
#endif

	if (m == NULL)
	{
		goto error;
	}

	JCDHT_install_dict();
	
	// Initialize toxcore
	if (PyType_Ready(&JCDHTType) < 0)
	{
		fprintf(stderr, "Invalid PyTypeObject `JCDHTType'\n");
		goto error;
	}

	Py_INCREF(&JCDHTType);
	PyModule_AddObject(m, "DHT", (PyObject*)&JCDHTType);

	DHTError = PyErr_NewException("dht.DHTError", NULL, NULL);
	PyModule_AddObject(m, "DHTError", (PyObject*)DHTError);

#if PY_MAJOR_VERSION >= 3
	return m;
#endif

error:
#if PY_MAJOR_VERSION >= 3
	return NULL;
#else
	return;
#endif
}
