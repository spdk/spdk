import time
import pexpect
from pexpect import pxssh
from exception import TimeoutException, SSHConnectionException, SSHSessionDeadException
from logger import RED, GREEN

"""
Module handle ssh sessions between tester and DUT.
"""


class SSHPexpect(object):

    def __init__(self, host, username, password):
        self.magic_prompt = "MAGIC PROMPT"
        try:
            self.session = pxssh.pxssh()
            self.host = host
            self.username = username
            self.password = password
            if ':' in host:
                self.ip = host.split(':')[0]
                self.port = int(host.split(':')[1])
                self.session.login(self.ip, self.username,
                                   self.password, original_prompt='[$#>]',
                                   port=self.port, login_timeout=20)
            else:
                self.session.login(self.host, self.username,
                                   self.password, original_prompt='[$#>]')
            self.send_expect('stty -echo', '# ', timeout=2)
        except Exception as e:
            print RED(e)
            if getattr(self, 'port', None):
                suggestion = "\nSuggession: Check if the fireware on [ %s ] " % \
                    self.ip + "is stoped\n"
                print GREEN(suggestion)
            raise SSHConnectionException(host)

    def init_log(self, logger, name):
        self.logger = logger
        self.logger.info("ssh %s@%s" % (self.username, self.host))

    def send_expect_base(self, command, expected, timeout):
        self.clean_session()
        self.session.PROMPT = expected
        self.__sendline(command)
        self.__prompt(command, timeout)
        before = self.get_output_before()
        return before

    def send_expect(self, command, expected, timeout=15, verify=False):
        ret = self.send_expect_base(command, expected, timeout)
        if verify:
            ret_status = self.send_expect_base("echo $?", expected, timeout)
            if not int(ret_status):
                return ret
            else:
                self.logger.error("Command: %s failure!" % command)
                self.logger.error(ret)
                return int(ret_status)
        else:
            return ret

    def send_command(self, command, timeout=1):
        self.clean_session()
        self.__sendline(command)
        return self.get_session_before(timeout)

    def clean_session(self):
        self.get_session_before(timeout=0.01)

    def get_session_before(self, timeout=15):
        """
        Get all output before timeout
        """
        self.session.PROMPT = self.magic_prompt
        try:
            self.session.prompt(timeout)
        except Exception as e:
            pass
        before = self.get_output_before()
        self.__flush()
        return before

    def __flush(self):
        """
        Clear all session buffer
        """
        self.session.buffer = ""
        self.session.before = ""

    def __prompt(self, command, timeout):
        if not self.session.prompt(timeout):
            raise TimeoutException(command, self.get_output_all())

    def __sendline(self, command):
        if not self.isalive():
            raise SSHSessionDeadException(self.host)
        if len(command) == 2 and command.startswith('^'):
            self.session.sendcontrol(command[1])
        else:
            self.session.sendline(command)

    def get_output_before(self):
        if not self.isalive():
            raise SSHSessionDeadException(self.host)
        self.session.flush()
        before = self.session.before.rsplit('\r\n', 1)
        if before[0] == "[PEXPECT]":
            before[0] = ""

        return before[0]

    def get_output_all(self):
        self.session.flush()
        output = self.session.before
        output.replace("[PEXPECT]", "")
        return output

    def close(self, force=False):
        if force is True:
            self.session.close()
        else:
            if self.isalive():
                self.session.logout()

    def isalive(self):
        return self.session.isalive()

    def copy_file_to(self, src, dst="~/", password=''):
        """
        Sends a local file to a remote place.
        """
        command = 'scp {0} {1}@{2}:{3}'.format(
            src, self.username, self.host, dst)
        if ':' in self.host:
            command = 'scp -v -P {0} -o NoHostAuthenticationForLocalhost=yes {1} {2}@{3}:{4}'.format(
                str(self.port), src, self.username, self.ip, dst)
        else:
            command = 'scp -v {0} {1}@{2}:{3}'.format(
                src, self.username, self.host, dst)
        if password == '':
            self._spawn_scp(command, self.password)
        else:
            self._spawn_scp(command, password)

    def _spawn_scp(self, scp_cmd, password):
        self.logger.info(scp_cmd)
        p = pexpect.spawn(scp_cmd)
        time.sleep(0.5)
        ssh_newkey = 'Are you sure you want to continue connecting'
        i = p.expect([ssh_newkey, '[pP]assword', "# ", pexpect.EOF,
                      pexpect.TIMEOUT], 120)
        if i == 0:
            p.sendline('yes')
            i = p.expect([ssh_newkey, '[pP]assword', pexpect.EOF], 2)
        if i == 1:
            time.sleep(0.5)
            p.sendline(password)
            p.expect("Exit status 0", 60)
        if i == 4:
            self.logger.error("SCP TIMEOUT error %d" % i)
        p.close()
