import binascii, time
from dht import DHT

class TestDHT(DHT):
	def loop(self):
		try:
			print "dht.transmissionbt.com:", self.ping("91.121.60.42", 6881)
			print "router.bittorent.com:", self.ping("67.215.242.138", 6881)
			while True:
				self.do()
				if time.clock() > 20:
					print "starting search"
					self.search(testhash)
		except KeyboardInterrupt:
			pass
	def on_search(self, ev, infohash, data):
		print "Event", repr(ev)
		print "Hash", repr(infohash)
		print "Data", repr(data)
	
testid = '\x12\xc6\xbf\xdf\xdf\x88\xa6\x18\xff\xb6\xc89\xc4j\xed2l\xa4\x98\x9b'
testport = 8000
testhash = binascii.a2b_hex('f919bc3c68eed436c485a564c51984a35af9151c')
a = TestDHT(testid, testport)
a.loop()
