from __future__ import print_function

from _devbuild.gen.runtime_asdl import scope_e
from _devbuild.gen.value_asdl import value, value_e, value_t
from builtin import func_native
from core.error import e_die
from core import pyos
from core import pyutil
from core import optview
from core import state
from frontend import location
from mycpp.mylib import iteritems, tagswitch, log
from osh import split
from pylib import os_path

import libc
import posix_ as posix

from typing import List, Dict, Optional, cast, TYPE_CHECKING
if TYPE_CHECKING:
    from _devbuild.gen import arg_types

_ = log


class EnvConfig(object):
    """Manage shell config from the environment, for OSH and YSH.

    Variables managed:

    PATH aka ENV.PATH     - where to look for executables
    PS1                   - how to print the prompt
    HISTFILE YSH_HISTFILE - where to read/write command history
    HOME                  - for ~ expansion (default not set)

    Features TODO

    - On-demand BASHPID
      - io.thisPid() - is BASHPID
      - io.pid() - is $$
    - Init-once UID EUID PPID
      - maybe this should be a separate Funcs class?
      - io.uid() io.euid() io.ppid()
    """

    def __init__(self, mem, defaults):
        # type: (state.Mem, Dict[str, value_t]) -> None
        self.mem = mem
        self.exec_opts = mem.exec_opts
        self.defaults = defaults

    def GetVal(self, var_name):
        # type: (str) -> value_t
        """
        YSH: Look at ENV.PATH, and then __defaults__.PATH
        OSH: Look at $PATH
        """
        if self.mem.exec_opts.env_obj():  # e.g. $[ENV.PATH]

            val = self.mem.env_dict.get(var_name)
            if val is None:
                val = self.defaults.get(var_name)

            if val is None:
                return value.Undef

        else:  # e.g. $PATH
            val = self.mem.GetValue(var_name)

        return val

    def Get(self, var_name):
        # type: (str) -> Optional[str]
        """
        Like GetVal(), but returns a strin, or None
        """
        val = self.GetVal(var_name)
        if val.tag() != value_e.Str:
            return None
        return cast(value.Str, val).s

    def SetDefault(self, var_name, s):
        # type: (str, str) -> None
        """
        OSH: Set HISTFILE var, which is read by GetVal()
        YSH: Set __defaults__.YSH_HISTFILE, which is also read by GetVal()
        """
        if self.mem.exec_opts.env_obj():  # e.g. $[ENV.PATH]
            self.mem.defaults[var_name] = value.Str(s)
        else:
            state.SetGlobalString(self.mem, var_name, s)


class ShellFiles(object):

    def __init__(self, lang, home_dir, mem, flag):
        # type: (str, str, state.Mem, arg_types.main) -> None
        assert lang in ('osh', 'ysh'), lang
        self.lang = lang
        self.home_dir = home_dir
        self.mem = mem
        self.flag = flag

        self.init_done = False

    def HistVar(self):
        # type: () -> str
        return 'HISTFILE' if self.lang == 'osh' else 'YSH_HISTFILE'

    def DefaultHistoryFile(self):
        # type: () -> str
        return os_path.join(self.home_dir,
                            '.local/share/oils/%s_history' % self.lang)

    def HistoryFile(self):
        # type: () -> Optional[str]
        assert self.init_done

        return self.mem.env_config.Get(self.HistVar())


def GetWorkingDir():
    # type: () -> str
    """Fallback for pwd builtin and $PWD when there's no 'cd' and no inherited $PWD."""
    try:
        return posix.getcwd()
    except (IOError, OSError) as e:
        e_die("Can't determine the working dir: %s" % pyutil.strerror(e))


# This was derived from bash --norc -c 'argv "$COMP_WORDBREAKS".
# Python overwrites this to something Python-specific in Modules/readline.c, so
# we have to set it back!
# Used in both core/competion.py and osh/state.py
_READLINE_DELIMS = ' \t\n"\'><=;|&(:'


def InitDefaultVars(mem, argv):
    # type: (state.Mem, List[str]) -> None

    # Problem: if you do shopt --set ysh:upgrade, the variable won't be
    # initialized.  So this is not different than lang == 'ysh'
    if mem.exec_opts.init_ysh_globals():  # YSH init
        # mem is initialized with a global frame
        mem.var_stack[0]['ARGV'] = state._MakeArgvCell(argv)

    # These 3 are special, can't be changed
    state.SetGlobalString(mem, 'UID', str(posix.getuid()))
    state.SetGlobalString(mem, 'EUID', str(posix.geteuid()))
    state.SetGlobalString(mem, 'PPID', str(posix.getppid()))

    # For getopts builtin - meant to be read, not changed
    state.SetGlobalString(mem, 'OPTIND', '1')

    # These can be changed.  Could go AFTER environment, e.g. in
    # InitVarsAfterEnv().

    # Default value; user may unset it.
    state.SetGlobalString(mem, 'IFS', ' \t\n')

    state.SetGlobalString(mem, 'HOSTNAME', libc.gethostname())

    # In bash, this looks like 'linux-gnu', 'linux-musl', etc.  Scripts test
    # for 'darwin' and 'freebsd' too.  They generally don't like at 'gnu' or
    # 'musl'.  We don't have that info, so just make it 'linux'.
    state.SetGlobalString(mem, 'OSTYPE', pyos.OsType())

    # When xtrace_rich is off, this is just like '+ ', the shell default
    state.SetGlobalString(mem, 'PS4',
                          '${SHX_indent}${SHX_punct}${SHX_pid_str} ')

    # bash-completion uses this.  Value copied from bash.  It doesn't integrate
    # with 'readline' yet.
    state.SetGlobalString(mem, 'COMP_WORDBREAKS', _READLINE_DELIMS)


def CopyVarsFromEnv(exec_opts, environ, mem):
    # type: (optview.Exec, Dict[str, str], state.Mem) -> None

    # POSIX shell behavior: env vars become exported global vars
    if not exec_opts.no_exported():
        for n, v in iteritems(environ):
            mem.SetNamed(location.LName(n),
                         value.Str(v),
                         scope_e.GlobalOnly,
                         flags=state.SetExport)

    # YSH behavior: env vars go in ENV dict, not exported vars.
    if exec_opts.env_obj():
        mem.MaybeInitEnvDict(environ)


def InitVarsAfterEnv(mem, mutable_opts):
    # type: (state.Mem, state.MutableOpts) -> None

    # If PATH SHELLOPTS PWD are not in environ, then initialize them.
    s = mem.env_config.Get('PATH')
    if s is None:
        mem.env_config.SetDefault('PATH', '/sbin:/usr/sbin:/bin:/usr/bin')

    if mem.exec_opts.no_init_globals():
        # YSH initialization
        mem.SetPwd(GetWorkingDir())
    else:
        # OSH initialization
        shellopts = mem.GetValue('SHELLOPTS')
        UP_shellopts = shellopts
        with tagswitch(shellopts) as case:
            if case(value_e.Str):
                shellopts = cast(value.Str, UP_shellopts)
                mutable_opts.InitFromEnv(shellopts.s)
            elif case(value_e.Undef):
                state.SetGlobalString(mem, 'SHELLOPTS',
                                      mutable_opts.ShelloptsString())
            else:
                raise AssertionError()

        mem.SetNamed(location.LName('SHELLOPTS'),
                     None,
                     scope_e.GlobalOnly,
                     flags=state.SetReadOnly)

        our_pwd = None  # type: Optional[str]
        val = mem.GetValue('PWD')
        UP_val = val
        with tagswitch(val) as case:
            if case(value_e.Str):
                val = cast(value.Str, UP_val)
                env_pwd = val.s
                if env_pwd.startswith('/') and pyos.IsSameFile(env_pwd, '.'):
                    our_pwd = env_pwd
            elif case(value_e.Undef):
                pass
            else:
                pass

        if our_pwd is None:
            our_pwd = GetWorkingDir()

        mem.SetNamed(location.LName('PWD'),
                     value.Str(our_pwd),
                     scope_e.GlobalOnly,
                     flags=state.SetExport)
        mem.SetPwd(our_pwd)


def InitInteractive(mem, sh_files, lang):
    # type: (state.Mem, ShellFiles, str) -> None
    """Initialization that's only done in the interactive/headless shell."""

    ps1_str = mem.env_config.Get('PS1')
    if ps1_str is None:
        mem.env_config.SetDefault('PS1', r'\s-\v\$ ')
    else:
        if lang == 'ysh':
            mem.env_dict['PS1'] = value.Str('ysh ' + ps1_str)

    hist_var = sh_files.HistVar()
    hist_str = mem.env_config.Get(hist_var)
    if hist_str is None:
        mem.env_config.SetDefault(hist_var, sh_files.DefaultHistoryFile())

    sh_files.init_done = True


def InitBuiltins(mem, version_str, defaults):
    # type: (state.Mem, str, Dict[str, value_t]) -> None
    """Initialize memory with shell defaults.

    Other interpreters could have different builtin variables.
    """
    # TODO: REMOVE this legacy.  ble.sh checks it!
    mem.builtins['OIL_VERSION'] = value.Str(version_str)

    mem.builtins['OILS_VERSION'] = value.Str(version_str)

    mem.builtins['__defaults__'] = value.Dict(defaults)

    # The source builtin understands '///' to mean "relative to embedded stdlib"
    mem.builtins['LIB_OSH'] = value.Str('///osh')
    mem.builtins['LIB_YSH'] = value.Str('///ysh')

    # - C spells it NAN
    # - JavaScript spells it NaN
    # - Python 2 has float('nan'), while Python 3 has math.nan.
    # - libc prints the strings 'nan' and 'inf'
    # - Python 3 prints the strings 'nan' and 'inf'
    # - JavaScript prints 'NaN' and 'Infinity', which is more stylized
    mem.builtins['NAN'] = value.Float(pyutil.nan())
    mem.builtins['INFINITY'] = value.Float(pyutil.infinity())

    # Native Grease actions are an explicit object rather than ambient C FFI.
    mem.builtins['native'] = func_native.MakeNativeObject()
