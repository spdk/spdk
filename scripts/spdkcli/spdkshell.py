import os
import six
import sys
from configshell_fb import shell
from configshell_fb import log
from configshell_fb import prefs
from configshell_fb import console
from pyparsing import (alphanums, Empty, Group, OneOrMore, Optional,
                       ParseResults, Regex, Suppress, Word, QuotedString, Or)


class SpdkShell(shell.ConfigShell):
    def __init__(self, preferences_dir=None):
        '''
        Creates a new ConfigShell.
        @param preferences_dir: Directory to load/save preferences from/to
        @type preferences_dir: str
        '''
        locatedExpr = shell.locatedExpr

        if sys.stdout.isatty():
            import readline
            tty = True
        else:
            tty = False

        self._current_node = None
        self._root_node = None
        self._exit = False

        # Grammar of the command line
        command = locatedExpr(Word(alphanums + '_'))('command')
        var = Word(alphanums + '_\+/.<>()~@:-%[]=')
        # value = var
        qvar = QuotedString('"', unquoteResults=True)
        value = Or([qvar, var])
        keyword = Word(alphanums + '_\-')
        kparam = locatedExpr(keyword + Suppress('=') + Optional(value, default=''))('kparams*')
        pparam = locatedExpr(value)('pparams*')
        parameter = kparam | pparam
        parameters = OneOrMore(parameter)
        bookmark = Regex('@([A-Za-z0-9:_.]|-)+')
        pathstd = Regex('([A-Za-z0-9:_.]|-)*' + '/' + '([A-Za-z0-9:_./]|-)*') | '..' | '.'
        path = locatedExpr(bookmark | pathstd | '*')('path')
        parser = Optional(path) + Optional(command) + Optional(parameters)
        self._parser = parser

        if tty:
            readline.set_completer_delims('\t\n ~!#$^&(){}\|;\'",?')
            readline.set_completion_display_matches_hook(
                self._display_completions)

        self.log = log.Log()

        if preferences_dir is not None:
            preferences_dir = os.path.expanduser(preferences_dir)
            if not os.path.exists(preferences_dir):
                os.makedirs(preferences_dir)
            self._prefs_file = preferences_dir + '/prefs.bin'
            self.prefs = prefs.Prefs(self._prefs_file)
            self._cmd_history = preferences_dir + '/history.txt'
            self._save_history = True
            if not os.path.isfile(self._cmd_history):
                try:
                    open(self._cmd_history, 'w').close()
                except IOError:
                    self.log.warning("Cannot create history file %s, "
                                     % self._cmd_history + "command history will not be saved.")
                    self._save_history = False

            if os.path.isfile(self._cmd_history) and tty:
                try:
                    readline.read_history_file(self._cmd_history)
                except IOError:
                    self.log.warning("Cannot read command history file %s."
                                     % self._cmd_history)

            if self.prefs['logfile'] is None:
                self.prefs['logfile'] = preferences_dir + '/' + 'log.txt'

            self.prefs.autosave = True

        else:
            self.prefs = prefs.Prefs()
            self._save_history = False

        try:
            self.prefs.load()
        except IOError:
            self.log.warning("Could not load preferences file %s."
                             % self._prefs_file)

        for pref, value in six.iteritems(self.default_prefs):
            if pref not in self.prefs:
                self.prefs[pref] = value

        self.con = console.Console()
