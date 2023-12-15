#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# pylint: disable=line-too-long, disable=invalid-name
# pylint: disable=broad-except, disable=too-many-statements
# pylint: disable=unsupported-membership-test, disable=too-many-branches

'''
    Test mDNS service browse and resolve.
'''
import argparse
import logging
import sys
import os
import time
import pexpect

def populate_services(console, num):
    console.sendline("mkdir -p /run/systemd/dnssd/")
    expectedSet = set()
    for i in range(num):
        expectedSet.add(str(populate_services.counter))
        filename="/run/systemd/dnssd/tst_service_" + str(i) + ".dnssd"
        console.sendline("echo [Service] > " + filename)
        console.sendline("echo Name=Test Service_" + str(populate_services.counter) + " >> " + filename)
        console.sendline("echo Type=_testService._udp >> " + filename)
        console.sendline("echo Port=24002 >> " + filename)
        console.sendline("echo TxtText=DC=Monitor PN=867313 SN=XZ051Z0051 >> " + filename)
        populate_services.counter += 1
    return expectedSet

def enableSystemdResolved(console):
    console.sendline("systemctl unmask systemd-resolved.service")
    console.expect("#", 5)
    console.sendline("systemctl enable --now systemd-resolved")
    console.expect("#", 5)
    console.sendline("echo $?")
    console.expect("0\r", 2)
    console.sendline("systemd-resolve --set-mdns=yes --interface=host0")
    console.expect("#", 5)
    console.sendline("echo $?")
    console.expect('0\r', 2)

def setupNetwork(console):
    console.sendline("systemctl unmask systemd-networkd")
    console.expect("#", 5)
    console.sendline("systemctl unmask systemd-networkd.socket")
    console.expect("#", 5)
    console.sendline("systemctl start systemd-networkd")
    console.expect("#", 5)
    console.sendline("systemctl start systemd-networkd.socket")
    console.expect("#", 5)

    console.sendline("/usr/lib/systemd/systemd-networkd-wait-online --ipv4 --ipv6 --interface=host0 --operational-state=degraded --timeout=30 ; echo NETWORKREADY")
    console.expect('NETWORKREADY', timeout=35)
    console.expect('NETWORKREADY', timeout=35)

def waitForNetworkUp(console):
    retval = -1
    retry = 0
    while(retry < 20 and retval != 0):
        console.sendline('networkctl | grep host0')
        retval = console.expect(['configured', pexpect.EOF, pexpect.TIMEOUT], 1)
        retry = retry+1

    if retry >= 20:
        raise Exception("Failed to configure network.\n")

def checkServices(console, expectedSet):
    actualSetv4 = set()
    actualSetv6 = set()
    for _ in range(len(expectedSet) * 2):
        console.expect('"add_flag":true,"family":(\\d+),"name":"Test Service_(\\d+)","type":"_testService._udp","domain":"local"', 30)
        proto = console.match.group(1)
        if '10' in proto:
            actualSetv6.add(console.match.group(2))
        else:
            actualSetv4.add(console.match.group(2))

    if actualSetv4 != expectedSet:
        raise Exception("Exception in adding services. Not all services were added.")
    if actualSetv6 != expectedSet:
        raise Exception("Exception in adding services. Not all services were added.")

def checkServicesRemoved(console, expectedSet):
    actualSetv4 = set()
    actualSetv6 = set()
    for _ in range(len(expectedSet) * 2):
        console.expect('"add_flag":false,"family":(\\d+),"name":"Test Service_(\\d+)","type":"_testService._udp","domain":"local"', 30)
        proto = console.match.group(1)
        if '10' in proto:
            actualSetv6.add(console.match.group(2))
        else:
            actualSetv4.add(console.match.group(2))

    if actualSetv4 != expectedSet:
        raise Exception("Exception in adding services. Not all services were removed.")
    if actualSetv6 != expectedSet:
        raise Exception("Exception in adding services. Not all services were removed.")

def terminateContainer(console):
    console.terminate()
    for _ in range(10):
        if not console.isalive():
            break

        time.sleep(1)
    else:
        # We haven't exited the loop early, so check if the process is
        # still alive - if so, force-kill it.
        if console.isalive():
            console.terminate(force=True)

def run_test(args):
    '''
    Run mDNS browse tests.
    Returns 0 if passed, 1 if any step fails.
    '''

    for x in args.arg:
        if "--machine" in x:
            args.arg.remove(x)
            machinearg=x
        elif "--directory" in x:
            args.arg.remove(x)
            rootpath=x

    machineName=machinearg.split('=')[1]
    rootdir=rootpath.split('=')[1].removesuffix('/root')

    args.arg.insert(0,"--network-bridge=br0")
    logger = logging.getLogger("test-resolved-mdns")

    retval = os.system("ip link show br0")
    if retval != 0:
        os.system("ip link add br0 type bridge")
    os.system("ip link set br0 up")

    os.system('mkdir -p '+rootdir+'/tmproot')
    os.system('cp -r '+rootdir+'/root '+rootdir+'/tmproot')
    os.system('cp '+rootdir+'/resolved_mdns.img '+rootdir+'/tmproot')

    os.system('mkdir -p '+rootdir+'/tmproot2')
    os.system('cp -r '+rootdir+'/root '+rootdir+'/tmproot2')
    os.system('cp '+rootdir+'/resolved_mdns.img '+rootdir+'/tmproot2')

    logger.info("Spawning container 1")

    arguments = args.arg
    arguments.insert(0, rootpath)
    arguments.insert(0, "--machine="+machineName+"-1")
    console = pexpect.spawn(args.command, arguments, env={
        "TERM": "linux",
    }, encoding='utf-8', timeout=60)
    time.sleep(2)
    console.logfile = sys.stdout
    logger.debug("child pid %d", console.pid)

    try:
        logger.info("waiting for login prompt")
        console.expect('H login: ', 10)
        logger.info("log in and start screen")
        console.sendline('root')
        console.expect('bash.*# ', 10)
    except Exception as e:
        logger.error(e)
        terminateContainer(console)
        return 1

    args.arg = [i for i in args.arg if '--directory' not in i]
    args.arg = [i for i in args.arg if '--machine' not in i]

    arguments = args.arg
    rootpath=rootpath.removesuffix('/root')
    arguments.insert(0, rootpath+'/tmproot/root')
    arguments.insert(0, "--machine="+machineName+"-2")

    logger.info("Spawning container 2")
    console2 = pexpect.spawn(args.command, arguments, env={
        "TERM": "linux",
    }, encoding='utf-8', timeout=60)
    time.sleep(2)
    console2.logfile = sys.stdout
    logger.debug("child pid %d", console2.pid)

    try:
        logger.info("waiting for login prompt")
        console2.expect('H login: ', 10)
        logger.info("log in and start screen")
        console2.sendline('root')
        console2.expect('bash.*# ', 10)
    except Exception as e:
        logger.error(e)
        terminateContainer(console2)
        return 1

    args.arg = [i for i in args.arg if '--directory' not in i]
    args.arg = [i for i in args.arg if '--machine' not in i]

    arguments = args.arg
    rootpath=rootpath.removesuffix('/root')
    arguments.insert(0, rootpath+'/tmproot2/root')
    arguments.insert(0, "--machine="+machineName+"-3")

    logger.info("Spawning container 3")
    console3 = pexpect.spawn(args.command, arguments, env={
        "TERM": "linux",
    }, encoding='utf-8', timeout=60)
    time.sleep(2)
    console3.logfile = sys.stdout
    logger.debug("child pid %d", console3.pid)

    try:
        logger.info("waiting for login prompt")
        console3.expect('H login: ', 10)
        logger.info("log in and start screen")
        console3.sendline('root')
        console3.expect('bash.*# ', 10)
    except Exception as e:
        logger.error(e)
        terminateContainer(console3)
        return 1

    try:
        logger.info('Setting up container 1')
        setupNetwork(console)
        enableSystemdResolved(console)

        logger.info('Setting up container 2')
        setupNetwork(console2)
        enableSystemdResolved(console2)

        logger.info('Setting up container 3')
        setupNetwork(console3)
        enableSystemdResolved(console3)

        expectedSet1 = populate_services(console, 3)
        expectedSet2 = populate_services(console2, 3)
        expectedSet3 = populate_services(console3, 3)

        # Wait for the network interface to be configured. Timeout after 20 seconds
        waitForNetworkUp(console)
        waitForNetworkUp(console2)
        waitForNetworkUp(console3)

        logger.info("Network configured")

        console.sendline("systemctl restart systemd-resolved")
        console.expect("#", 5)
        console2.sendline("systemctl restart systemd-resolved")
        console2.expect("#", 5)
        console3.sendline("systemctl restart systemd-resolved")
        console3.expect("#", 5)

        # TEST 1 - call StartBrowse on console2, while all containers are publishing mDNS services.
        logger.info("Running TEST 1")
        console2.sendline("varlinkctl call --more /run/systemd/resolve/io.systemd.Resolve io.systemd.Resolve.StartBrowse \'{\"domainName\":\"_testService._udp.local\",\"name\":\"\",\"type\":\"\",\"ifindex\":2,\"flags\":16785432}\' &")
        checkServices(console2, expectedSet1.union(expectedSet3))
        time.sleep(3)

        # TEST 2 - testing calling startbrowse multiple times
        # this also tests service announcements on startup and service removals with goodbye packets
        logger.info("Running TEST 2")
        console2.sendline("varlinkctl call --more /run/systemd/resolve/io.systemd.Resolve io.systemd.Resolve.StartBrowse \'{\"domainName\":\"_testService._udp.local\",\"name\":\"\",\"type\":\"\",\"ifindex\":2,\"flags\":16785432}\'")
        console2.expect("Method call failed: io.systemd.System", 5)
        console2.expect("\"errno\":16", 5)
        console.sendline("systemctl stop systemd-resolved")
        console.expect("#")
        checkServicesRemoved(console2, expectedSet1)
        console.sendline("systemctl start systemd-resolved")
        console.expect("#")
        checkServices(console2, expectedSet1)

        console3.sendline("varlinkctl call --more /run/systemd/resolve/io.systemd.Resolve io.systemd.Resolve.StartBrowse \'{\"domainName\":\"_testService._udp.local\",\"name\":\"\",\"type\":\"\",\"ifindex\":2,\"flags\":16785432}\'")
        checkServices(console3, expectedSet1.union(expectedSet2))

        # All three devices are browsing and publishing simultaneously after this
        console.sendline("varlinkctl call --more /run/systemd/resolve/io.systemd.Resolve io.systemd.Resolve.StartBrowse \'{\"domainName\":\"_testService._udp.local\",\"name\":\"\",\"type\":\"\",\"ifindex\":2,\"flags\":16785432}\'")
        checkServices(console, expectedSet2.union(expectedSet3))

        # TEST 3 - Network interface down
        logger.info("Running TEST 3")
        console2.sendline("ip link set host0 down")
        checkServicesRemoved(console2, expectedSet1.union(expectedSet3))
        console2.sendline("ip link set host0 up")
        # Takes a few seconds for the iface to get an IPv4 addr
        waitForNetworkUp(console2)
        checkServices(console2, expectedSet1.union(expectedSet3))

        # Restart varlinkctl to avoid timeout
        console.sendcontrol('c')
        console.expect("#", 5)
        console.sendline("varlinkctl call --more /run/systemd/resolve/io.systemd.Resolve io.systemd.Resolve.StartBrowse \'{\"domainName\":\"_testService._udp.local\",\"name\":\"\",\"type\":\"\",\"ifindex\":2,\"flags\":16785432}\' &")
        console.sendline("ip link set host0 down")
        checkServicesRemoved(console, expectedSet2.union(expectedSet3))
        console.sendline("ip link set host0 up")
        # Takes a few seconds for the iface to get an IPv4 addr
        waitForNetworkUp(console)
        checkServices(console, expectedSet2.union(expectedSet3))

        # Restart varlinkctl to avoid timeout
        console3.sendcontrol('c')
        console3.expect("#", 5)
        console3.sendline("varlinkctl call --more /run/systemd/resolve/io.systemd.Resolve io.systemd.Resolve.StartBrowse \'{\"domainName\":\"_testService._udp.local\",\"name\":\"\",\"type\":\"\",\"ifindex\":2,\"flags\":16785432}\' &")
        console3.sendline("ip link set host0 down")
        checkServicesRemoved(console3, expectedSet1.union(expectedSet2))
        console3.sendline("ip link set host0 up")
        # Takes a few seconds for the iface to get an IPv4 addr
        waitForNetworkUp(console3)
        checkServices(console3, expectedSet1.union(expectedSet2))

        console.sendcontrol('c')
        console.expect("#", 5)
        console.sendline('> /testok')
        console.sendline("poweroff")
        console.expect(pexpect.EOF, 3)

        console2.sendcontrol('c')
        console2.expect("#", 5)
        console2.sendline("poweroff")
        console2.expect(pexpect.EOF, 3)

        console3.sendcontrol('c')
        console3.expect("#", 5)
        console3.sendline("poweroff")
        console3.expect(pexpect.EOF, 3)

        ret = 0

    except Exception as e:
        ret = 1
        logger.error(e)
        console.sendcontrol('c')
        console.sendline("poweroff")
        console2.sendcontrol('c')
        console2.sendline("poweroff")
        console3.sendcontrol('c')
        console3.sendline("poweroff")
        time.sleep(1)
        logger.info("killing child pid %d", console.pid)
        console.terminate()
        logger.info("killing child pid %d", console2.pid)
        console2.terminate()
        logger.info("killing child pid %d", console3.pid)
        console3.terminate()

    return ret

def main():
    parser = argparse.ArgumentParser(description='test resolved mdns browse feature')
    parser.add_argument("-v", "--verbose", action="store_true", help="verbose")
    parser.add_argument("command", help="command to run")
    parser.add_argument("arg", nargs='*', help="args for command")

    args = parser.parse_args()

    if args.verbose:
        level = logging.DEBUG
    else:
        level = logging.INFO

    logging.basicConfig(level=level)

    return run_test(args)

if __name__ == "__main__":
    populate_services.counter = 0
    sys.exit(main())
