#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (c) 2022 Koninklijke Philips N.V.
# pylint: disable=line-too-long, disable=invalid-name
# pylint: disable=broad-except, disable=too-many-statements

'''
    Test mDNS service browse and resolve.
'''
import logging
import sys
import os
import time
import pexpect
from mdns_publisher import publish_mdns_service, remove_services, get_ipv4_addr, get_ipv6_addr

def run_test():
    '''
    Run mDNS browse and resolve tests.
    Returns 0 if passed, 1 if any step fails.
    '''
    ret = 1
    logger = logging.getLogger("test-shutdown")
    txt_string = ("\"rm=\" \"ve=05\" \"md=Chromecast Ultra\" \"ic=/setup/icon.png\""
            " \"fn=LivingRoom\" \"ca=4101\" \"st=0\" \"bs=FA8FCA7EO948\" \"rs=0\"")

    logger.info("spawning test")
    console = pexpect.spawn("systemd-nspawn -M testcont -bi ../image.raw --network-veth", [], env={
        "TERM": "linux",
    }, encoding='utf-8', timeout=30)
    time.sleep(2)
    console.logfile = sys.stdout

    logger.debug("child pid %d", console.pid)
    try:
        console.sendline("uname -n")
        console.expect("testcont")

        # Setup
        logger.info("configure network")
        os.system("ip link set ve-testcont up")
        time.sleep(1)
        console.expect("#", 1)
        console.sendline("systemctl enable --now systemd-resolved")
        console.expect("#", 1)
        console.sendline("echo $?")
        console.expect("0\r", 2)
        console.sendline("systemd-resolve --set-mdns=yes --interface=host0")
        console.expect("#", 1)
        console.sendline("echo $?")
        console.expect('0\r', 2)
        time.sleep(20)

        # Test continuous browse and resolve.
        # (1) Publish 10 instances of mDNS service and check if services are listed along with
        #     resolved data.
        # (2) Then remove all 10 instances by sending goodbye packets and check if notifications of
        #     removal are received.
        # (3) Repeat 1 and 2.
        console.sendline("mdns-browse-services --domain _googlecast._tcp.local --interface host0"
                        " --resolve")
        source_ip4=get_ipv4_addr('ve-testcont')
        source_ip6=get_ipv6_addr('ve-testcont')
        for _ in range(2):
            id_list = publish_mdns_service(100, 10, 0)
            for s in id_list:
                console.expect("\\+  host0 AF_INET Chromecast-Ultra-"+s+" _googlecast._tcp     local", 10)
                console.expect("hostname = \\["+s+".local\\]")
                console.expect("port = \\[8009\\]")
                console.expect("address = \\["+source_ip4+"\\]")
                console.expect("txt = \\[\"id="+s+"\" "+txt_string+"\\]")
                console.expect("\\+  host0 AF_INET6 Chromecast-Ultra-"+s+" _googlecast._tcp     local", 10)
                console.expect("hostname = \\["+s+".local\\]")
                console.expect("port = \\[8009\\]")
                console.expect("address = \\["+source_ip6+"\\]")
                console.expect("txt = \\[\"id="+s+"\" "+txt_string+"\\]")
            logger.info("Removing services..")
            remove_services(id_list)
            for s in id_list:
                console.expect("\\-  host0 AF_INET Chromecast-Ultra-"+s+" _googlecast._tcp     local", 10)
                console.expect("\\-  host0 AF_INET6 Chromecast-Ultra-"+s+" _googlecast._tcp     local", 10)
        console.sendcontrol('c')
        console.expect("#", 5)
        time.sleep(2)

        # Test cache maintenance.
        # Publish 5 instances of mDNS service with 5 seconds between each.
        # Check if notifications of removal are received when TTL of each instance expires.
        console.sendline("mdns-browse-services --domain _googlecast._tcp.local --interface host0")
        id_list = publish_mdns_service(20, 5, 5)
        logger.info("Test cache maintenance..")
        console.expect("\\+  host0 AF_INET Chromecast-Ultra-"+id_list[0]+" _googlecast._tcp     local", 6)
        for s in id_list:
            console.expect("\\-  host0 (\\w+) Chromecast-Ultra-"+s+" _googlecast._tcp     local", 21)
            proto = console.match.group(1)
            proto1 = "AF_INET" if "AF_INET6" in proto else "AF_INET6"
            console.expect("\\-  host0 "+ proto1 + " Chromecast-Ultra-"+s+" _googlecast._tcp     local", 5)
        console.sendcontrol('c')
        console.expect("#", 5)
        console.sendline("poweroff")
        console.expect(pexpect.EOF, 3)
        ret = 0
    except Exception as e:
        console.sendcontrol('c')
        console.sendline("poweroff")
        time.sleep(1)
        logger.error(e)
        logger.info("killing child pid %d", console.pid)
        console.terminate()

    return ret

if __name__ == "__main__":
    run_test()
