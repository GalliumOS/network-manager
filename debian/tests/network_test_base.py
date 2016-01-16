'''
Base class for network related tests.

This provides fake wifi devices with mac80211_hwsim and hostapd, test ethernet
devices with veth, utility functions to start wpasupplicant, dnsmasq, get/set
rfkill status, and some utility functions.
'''

__author__ = 'Martin Pitt <martin.pitt@ubuntu.com>'
__copyright__ = '(C) 2013 Canonical Ltd.'
__license__ = 'GPL v2 or later'

import sys
import os
import os.path
import time
import tempfile
import subprocess
import re
import unittest
import traceback
import functools
from glob import glob

# check availability of programs, and cleanly skip test if they are not
# available
for program in ['wpa_supplicant', 'hostapd', 'dnsmasq', 'dhclient']:
    if subprocess.call(['which', program], stdout=subprocess.PIPE) != 0:
        sys.stderr.write('%s is required for this test suite, but not available. Skipping\n' % program)
        sys.exit(0)


class NetworkTestBase(unittest.TestCase):
    '''Common functionality for network test cases

    setUp() creates two test wlan devices, one for a simulated access point
    (self.dev_w_ap), the other for a simulated client device
    (self.dev_w_client), and two test ethernet devices (self.dev_e_ap and
    self.dev_e_client).

    Each test should call self.setup_ap() or self.setup_eth() with the desired
    configuration.
    '''
    @classmethod
    def setUpClass(klass):
        # ensure we have this so that iw works
        subprocess.check_call(['modprobe', 'cfg80211'])

        # set regulatory domain "EU", so that we can use 80211.a 5 GHz channels
        out = subprocess.check_output(['iw', 'reg', 'get'], universal_newlines=True)
        m = re.match('^country (\S+):', out)
        assert m
        klass.orig_country = m.group(1)
        subprocess.check_call(['iw', 'reg', 'set', 'EU'])

    @classmethod
    def tearDownClass(klass):
        subprocess.check_call(['iw', 'reg', 'set', klass.orig_country])

    @classmethod
    def create_devices(klass):
        '''Create Access Point and Client devices with mac80211_hwsim and veth'''

        if os.path.exists('/sys/module/mac80211_hwsim'):
            raise SystemError('mac80211_hwsim module already loaded')
        if os.path.exists('/sys/class/net/eth42'):
            raise SystemError('eth42 interface already exists')

        # create virtual ethernet devs
        subprocess.check_call(['ip', 'link', 'add', 'name', 'eth42', 'type',
                               'veth', 'peer', 'name', 'veth42'])
        klass.dev_e_ap = 'veth42'
        klass.dev_e_client = 'eth42'

        # create virtual wlan devs
        before_wlan = set([c for c in os.listdir('/sys/class/net') if c.startswith('wlan')])
        subprocess.check_call(['modprobe', 'mac80211_hwsim'])
        # wait 5 seconds for fake devices to appear
        timeout = 50
        while timeout > 0:
            after_wlan = set([c for c in os.listdir('/sys/class/net') if c.startswith('wlan')])
            if len(after_wlan) - len(before_wlan) >= 2:
                break
            timeout -= 1
            time.sleep(0.1)
        else:
            raise SystemError('timed out waiting for fake devices to appear')

        devs = list(after_wlan - before_wlan)
        klass.dev_w_ap = devs[0]
        klass.dev_w_client = devs[1]

        # determine and store MAC addresses
        with open('/sys/class/net/%s/address' % klass.dev_w_ap) as f:
            klass.mac_w_ap = f.read().strip().upper()
        with open('/sys/class/net/%s/address' % klass.dev_w_client) as f:
            klass.mac_w_client = f.read().strip().upper()
        with open('/sys/class/net/%s/address' % klass.dev_e_ap) as f:
            klass.mac_e_ap = f.read().strip().upper()
        with open('/sys/class/net/%s/address' % klass.dev_e_client) as f:
            klass.mac_e_client = f.read().strip().upper()
        #print('Created fake devices: AP: %s, client: %s' % (klass.dev_w_ap, klass.dev_w_client))

    @classmethod
    def shutdown_devices(klass):
        '''Remove test wlan devices'''

        subprocess.check_call(['rmmod', 'mac80211_hwsim'])
        subprocess.check_call(['ip', 'link', 'del', 'dev', klass.dev_e_ap])
        klass.dev_w_ap = None
        klass.dev_w_client = None
        klass.dev_e_ap = None
        klass.dev_e_client = None

    @classmethod
    def get_rfkill(klass, interface):
        '''Get rfkill status of an interface.

        Returns whether the interface is blocked, i. e. "True" for blocked,
        "False" for enabled.
        '''
        with open(klass._rfkill_attribute(interface)) as f:
            val = f.read()
        return val == '1'

    @classmethod
    def set_rfkill(klass, interface, block):
        '''Set rfkill status of an interface

        Use block==True for disabling ("killswitching") an interface,
        block==False to re-enable.
        '''
        with open(klass._rfkill_attribute(interface), 'w') as f:
            f.write(block and '1' or '0')

    def run(self, result=None):
        '''Show log files on failed tests'''

        if result:
            orig_err_fail = len(result.errors) + len(result.failures)
        super().run(result)
        if hasattr(self, 'workdir'):
            logs = glob(os.path.join(self.workdir, '*.log'))
            if result and len(result.errors) + len(result.failures) > orig_err_fail:
                for log_file in logs:
                    with open(log_file) as f:
                        print('\n----- %s -----\n%s\n------\n'
                              % (os.path.basename(log_file), f.read()))

            # clean up log files, so that we don't see ones from previous tests
            for log_file in logs:
                os.unlink(log_file)

    def setUp(self):
        '''Create test devices and workdir'''

        self.create_devices()
        self.addCleanup(self.shutdown_devices)
        self.workdir_obj = tempfile.TemporaryDirectory()
        self.workdir = self.workdir_obj.name

        # create static entropy file to avoid draining/blocking on /dev/random
        self.entropy_file = os.path.join(self.workdir, 'entropy')
        with open(self.entropy_file, 'wb') as f:
            f.write(b'012345678901234567890')

    def setup_ap(self, hostapd_conf, ipv6_mode):
        '''Set up simulated access point

        On self.dev_w_ap, run hostapd with given configuration. Setup dnsmasq
        according to ipv6_mode, see start_dnsmasq().

        This is torn down automatically at the end of the test.
        '''
        # give our AP an IP
        subprocess.check_call(['ip', 'a', 'flush', 'dev', self.dev_w_ap])
        if ipv6_mode is not None:
            subprocess.check_call(['ip', 'a', 'add', '2600::1/64', 'dev', self.dev_w_ap])
        else:
            subprocess.check_call(['ip', 'a', 'add', '192.168.5.1/24', 'dev', self.dev_w_ap])

        self.start_hostapd(hostapd_conf)
        self.start_dnsmasq(ipv6_mode, self.dev_w_ap)

    def setup_eth(self, ipv6_mode, start_dnsmasq=True):
        '''Set up simulated ethernet router

        On self.dev_e_ap, run dnsmasq according to ipv6_mode, see
        start_dnsmasq().

        This is torn down automatically at the end of the test.
        '''
        # give our router an IP
        subprocess.check_call(['ip', 'a', 'flush', 'dev', self.dev_e_ap])
        if ipv6_mode is not None:
            subprocess.check_call(['ip', 'a', 'add', '2600::1/64', 'dev', self.dev_e_ap])
        else:
            subprocess.check_call(['ip', 'a', 'add', '192.168.5.1/24', 'dev', self.dev_e_ap])
        subprocess.check_call(['ip', 'link', 'set', self.dev_e_ap, 'up'])
        # we don't really want to up the client iface already, but veth doesn't
        # work otherwise (no link detected)
        subprocess.check_call(['ip', 'link', 'set', self.dev_e_client, 'up'])

        if start_dnsmasq:
            self.start_dnsmasq(ipv6_mode, self.dev_e_ap)

    def start_wpasupp(self, conf):
        '''Start wpa_supplicant on client interface'''

        w_conf = os.path.join(self.workdir, 'wpasupplicant.conf')
        with open(w_conf, 'w') as f:
            f.write('ctrl_interface=%s\nnetwork={\n%s\n}\n' % (self.workdir, conf))
        log = os.path.join(self.workdir, 'wpasupp.log')
        p = subprocess.Popen(['wpa_supplicant', '-Dwext', '-i', self.dev_w_client,
                              '-e', self.entropy_file, '-c', w_conf, '-f', log],
                             stderr=subprocess.PIPE)
        self.addCleanup(p.wait)
        self.addCleanup(p.terminate)
        # TODO: why does this sometimes take so long?
        self.poll_text(log, 'CTRL-EVENT-CONNECTED', timeout=200)

    def wrap_process(self, fn, *args, **kwargs):
        '''Run a test method in a separate process.

        Run test method fn(*args, **kwargs) in a child process. If that raises
        any exception, it gets propagated to the main process and
        wrap_process() fails with that exception.
        '''
        # exception from subprocess is propagated through this file
        exc_path = os.path.join(self.workdir, 'exc')
        try:
            os.unlink(exc_path)
        except OSError:
            pass

        pid = os.fork()

        # run the actual test in the child
        if pid == 0:
            # short-circuit tearDownClass(), as this will be done by the parent
            # process
            self.addCleanup(os._exit, 0)
            try:
                fn(*args, **kwargs)
            except:
                with open(exc_path, 'w') as f:
                    f.write(traceback.format_exc())
                raise
        else:
            # get success/failure state from child
            os.waitpid(pid, 0)
            # propagate exception
            if os.path.exists(exc_path):
                with open(exc_path) as f:
                    self.fail(f.read())

    #
    # Internal implementation details
    #

    @classmethod
    def poll_text(klass, logpath, string, timeout=50):
        '''Poll log file for a given string with a timeout.

        Timeout is given in deciseconds.
        '''
        log = ''
        while timeout > 0:
            if os.path.exists(logpath):
                break
            timeout -= 1
            time.sleep(0.1)
        assert timeout > 0, 'Timed out waiting for file %s to appear' % logpath

        with open(logpath) as f:
            while timeout > 0:
                line = f.readline()
                if line:
                    log += line
                    if string in line:
                        break
                    continue
                timeout -= 1
                time.sleep(0.1)

        assert timeout > 0, 'Timed out waiting for "%s":\n------------\n%s\n-------\n' % (string, log)

    def start_hostapd(self, conf):
        hostapd_conf = os.path.join(self.workdir, 'hostapd.conf')
        with open(hostapd_conf, 'w') as f:
            f.write('interface=%s\ndriver=nl80211\n' % self.dev_w_ap)
            f.write(conf)

        log = os.path.join(self.workdir, 'hostapd.log')
        p = subprocess.Popen(['hostapd', '-e', self.entropy_file, '-f', log, hostapd_conf],
                             stdout=subprocess.PIPE)
        self.addCleanup(p.wait)
        self.addCleanup(p.terminate)
        self.poll_text(log, '' + self.dev_w_ap + ': AP-ENABLED')

    def start_dnsmasq(self, ipv6_mode, iface):
        '''Start dnsmasq.

        If ipv6_mode is None, IPv4 is set up with DHCP. If it is not None, it
        must be a valid dnsmasq mode, i. e. a combination of "ra-only",
        "slaac", "ra-stateless", and "ra-names". See dnsmasq(8).
        '''
        if ipv6_mode is None:
            dhcp_range = '192.168.5.10,192.168.5.200'
        else:
            dhcp_range = '2600::10,2600::20'
            if ipv6_mode:
                dhcp_range += ',' + ipv6_mode

        self.dnsmasq_log = os.path.join(self.workdir, 'dnsmasq.log')
        lease_file = os.path.join(self.workdir, 'dnsmasq.leases')

        p = subprocess.Popen(['dnsmasq', '--keep-in-foreground', '--log-queries',
                              '--log-facility=' + self.dnsmasq_log,
                              '--conf-file=/dev/null',
                              '--dhcp-leasefile=' + lease_file,
                              '--bind-interfaces',
                              '--interface=' + iface,
                              '--except-interface=lo',
                              '--enable-ra',
                              '--dhcp-range=' + dhcp_range])
        self.addCleanup(p.wait)
        self.addCleanup(p.terminate)

        if ipv6_mode is not None:
            self.poll_text(self.dnsmasq_log, 'IPv6 router advertisement enabled')
        else:
            self.poll_text(self.dnsmasq_log, 'DHCP, IP range')

    @classmethod
    def _rfkill_attribute(klass, interface):
        '''Return the path to interface's rfkill soft toggle in sysfs.'''

        g = glob('/sys/class/net/%s/phy80211/rfkill*/soft' % interface)
        assert len(g) == 1, 'Did not find exactly one "soft" rfkill attribute for %s: %s' % (
            interface, str(g))
        return g[0]


def run_in_subprocess(fn):
    '''Decorator for running fn in a child process'''

    @functools.wraps(fn)
    def wrapped(*args, **kwargs):
        # args[0] is self
        args[0].wrap_process(fn, *args, **kwargs)
    return wrapped
