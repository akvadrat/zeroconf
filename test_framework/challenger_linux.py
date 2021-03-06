#!/usr/bin/python


# challenger_linux: Implementation of the zeroconf test challenger for linux.
# This module only works for a local linux implementation, but could probably be
# extended to operate over ssh, telnet, or whatever.
#

import sys
from zc_base import challenger_base
from zc_base import zc_test_exception
import os
import time
import socket
from dpkt import ethernet
import myarp
import myeth
from myarp import myarp
from mdns import mdns

class challenger_linux(challenger_base):
	def __init__( self, conf ):
		self.conf = conf
		return

	def __del__( self ):
		return

	def set_wifi( self, ssid="ANY", mode="managed", channel=6 ):
		iface = self.conf.CHALLENGER_INTERFACE
		cmd = "iwconfig " + iface + " mode " + mode
		cmd += " essid " + ssid
		if mode != "managed":
			cmd += " channel %d"%(channel)
			
		r = os.system(cmd)
		if r != 0:
			raise zc_test_exception, r

	def set_ip( self, ip="", netmask="", gateway="" ):
		if ip == "":
			ip = self.conf.MANAGED_CHALLENGER_IP
		if netmask == "":
			netmask = self.conf.MANAGED_NETMASK
		if gateway == "":
			gateway = self.conf.MANAGED_GW

		# set up IP stuff
		iface = self.conf.CHALLENGER_INTERFACE
		cmd = "ifconfig " + iface + " netmask " + netmask + " " + ip
		r = os.system(cmd)
		if r != 0:
			raise zc_test_exception, r
		return

	def get_mac(self):
		iface = self.conf.CHALLENGER_INTERFACE
		cmd = "ifconfig " + iface + " | grep HWaddr | awk '{print $5}'"
		r = os.popen(cmd).read()
		return r

	def ping( self, ip ):
		cmd = "ping -c 1 -I " + self.conf.CHALLENGER_INTERFACE + " " + ip
		if self.conf.DEBUG == 0:
			cmd += " >/dev/null"
		else:
			print cmd + "\n"
		r = os.system(cmd)
		if r == 0:
			return True
		else:
			return False

	def recv_arp( self, timeout ):
		s = socket.socket(socket.PF_PACKET, socket.SOCK_RAW)
		iface = self.conf.CHALLENGER_INTERFACE
		s.bind((iface, ethernet.ETH_TYPE_ARP))
		s.settimeout(timeout)
		data = s.recv(1024)
		answer = ethernet.Ethernet(data)
		arp_p = answer.data
		return myarp(arp_p)
	
	def send_arp( self, arp_p, eth):
		mac = self.get_mac()

		packet = ethernet.Ethernet()
		packet.src = myeth.eth_aton(mac)
		packet.dst = eth
		packet.data = arp_p
		packet.type = ethernet.ETH_TYPE_ARP

		s = socket.socket(socket.PF_PACKET, socket.SOCK_RAW)
		iface = self.conf.CHALLENGER_INTERFACE
		s.bind((iface, ethernet.ETH_TYPE_ARP))
		s.send(str(packet))

	def send_mdns_query( self, name ):
		m = mdns()
		if m.query( name, self.conf.MANAGED_CHALLENGER_IP ):
			return ''
		return 'no response'
