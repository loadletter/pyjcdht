import sys
from distutils.core import setup, Extension
from subprocess import Popen, PIPE

def supports_openssl():
	h = Popen("/sbin/ldconfig -p | grep openssl", shell=True, stdout=PIPE)
	out, err = h.communicate()
	return 'openssl' in str(out)

sources = ["src/dht.c", "src/core.c", "src/dht/dht.c"]
libraries = ["crypt"]
cflags = ["-g", "-Wall"]

if "--enable-verbose" in sys.argv:
	cflags.append("-DENABLE_VERBOSE")
	sys.argv.remove("--enable-verbose")

if supports_openssl():
	libraries.append("crypto")
	libraries.append("ssl")
	cflags.append("-DENABLE_OPENSSL")
else:
	print("Warning: OpenSSL support not found, using weaker crypto.")

setup(
	name="PyJCDHT",
	version="0.0.1",
	description = 'Python binding for the BitTorrent DHT implementation used by transmission',
	author = 'loadletter',
	author_email = 'loadletter@users.noreply.github.com',
	url = 'http://github.com/loadletter/pyjcdht',
	license = 'GPL',
	ext_modules=[
	   Extension("dht", sources,
	   extra_compile_args=cflags,
	   libraries=libraries)
	]
)
