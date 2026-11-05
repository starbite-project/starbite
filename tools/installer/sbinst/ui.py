"""The terminal UI: headings, status lines, tables, questions and typed confirmations. Everything
shown is also written to the session log."""
import os
import sys


class UI:
    def __init__(self, assume_yes=False, allow_yes_typed=False, color=None, out=None, inp=None):
        self.out = out or sys.stdout
        self.inp = inp or input
        self.assume_yes = assume_yes
        self.allow_yes_typed = allow_yes_typed       # only in --rehearse
        if color is None:
            color = hasattr(self.out, 'isatty') and self.out.isatty() and not os.environ.get('NO_COLOR')
        self.color = color
        self.log = None

    def _c(self, code, s):
        return f'\033[{code}m{s}\033[0m' if self.color else s

    def emit(self, s=''):
        print(s, file=self.out, flush=True)
        if self.log:
            self.log.write(s + '\n')
            self.log.flush()

    def banner(self):
        self.emit(self._c('1', '=' * 64))
        self.emit(self._c('1', '  STARBITE INSTALLER  -  Galaxy S9 (SM-G960F, starlte)'))
        self.emit(self._c('1', '=' * 64))

    def h1(self, s):
        self.emit()
        self.emit(self._c('1;36', f'== {s} =='))

    def h2(self, s):
        self.emit(self._c('1', f'-- {s}'))

    def info(self, s):
        self.emit(f'   {s}')

    def ok(self, s):
        self.emit(self._c('32', f'ok   {s}'))

    def warn(self, s):
        self.emit(self._c('33', f'WARN {s}'))

    def bad(self, s):
        self.emit(self._c('31', f'FAIL {s}'))

    def refused(self, s):
        self.emit(self._c('31', f'REFUSED: {s}'))

    def stopped(self, s):
        self.emit(self._c('31', f'STOPPED: {s}'))

    def cmd(self, s):
        self.emit(self._c('2', f'   $ {s}'))

    def table(self, rows, headers):
        rows = [[str(c) for c in r] for r in rows]
        w = [max(len(h), *(len(r[i]) for r in rows)) if rows else len(h) for i, h in enumerate(headers)]
        self.emit('   ' + '  '.join(h.ljust(w[i]) for i, h in enumerate(headers)))
        self.emit('   ' + '  '.join('-' * w[i] for i in range(len(headers))))
        for r in rows:
            self.emit('   ' + '  '.join(c.ljust(w[i]) for i, c in enumerate(r)))

    def ask(self, prompt):
        self.emit(prompt)
        try:
            a = self.inp('   > ')
        except EOFError:
            a = ''
        if self.log:
            self.log.write(f'   > {a}\n')
        return a.strip()

    def confirm(self, question):
        """y/N. --yes answers yes."""
        if self.assume_yes:
            self.emit(f'{question} [y/N] y (--yes)')
            return True
        return self.ask(f'{question} [y/N]').lower() in ('y', 'yes')

    def typed(self, prompt, expected):
        """The user must type `expected` exactly. --yes answers only in rehearsal."""
        if self.assume_yes and self.allow_yes_typed:
            self.emit(f'{prompt}\n   (rehearsal --yes typed: {expected})')
            return True
        return self.ask(f'{prompt}\n   Type exactly: {expected}') == expected
