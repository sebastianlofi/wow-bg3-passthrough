#!/usr/bin/env python3
"""wowaccount.py: one hidden WoW account per BG3 playthrough.

Why: a WoW account holds at most 10 characters on a realm (vanilla, CHAR_CREATE_SERVER_LIMIT 0x34), and
every BG3 New Game makes a new hero character, so with one account the 11th New Game got no WoW world
(charcreate's blocker B4). Now each New Game's hero character is made on an account of its own, made
for it at that moment; the player never sees it.

  * Account: WB3P + 8 random letters or digits, password 16 random letters or digits, made through the
    server console's own `account create` (link/tools/wowserver.py: harness A's test server, harness
    B's or the release's portable server), player rights (gmlevel 0). Companions are not here: the
    server's `bot add` gives every bot an account of its own (WB3B<NAME>, patch 0010), one character each.
  * Registry: accounts.json (mode 600) in the folder below, {"Accounts": {USER: {Pass, Made, Hero,
    Session}}}. Passwords are only there; nothing logs them.
  * Which account logs in: the one that owns the character in the server's database (account_for). A
    character on a WB3P account logs in with that account; any other (saves made before this change: the
    player's first account, WOWBG3TEST on the test servers) with the default account, as before.
  * Hand-over: the lifecycle passes the account as the last argument of its `lc benilla CHAR ACCOUNT` and
    `lc create SPEC ACCOUNT` actions (absent: the default account). The dev launchers hand the name to
    benilla's unit (WOW_BG3_ACCOUNT) and server/tools/launch-baseline.sh reads its password here
    (`wowaccount.py password ACCOUNT`); the release's coordinator calls password() in-process.
Folder: $WOWBG3_ACCOUNTS_DIR; else $WOWBG3_DATA (the release's data folder, ~/.local/share/wow-bg3, the
launcher plugin sets it); else the parent of $WOWBG3_SERVER_VAR (the same folder, for tools run in release
mode); else $WOW_BG3_SERVER/var (harness B's portable server); else server/private
(harness A's test server).
API (release's coordinator, py3.9): new(hero, session) -> account; account_for(char) -> account or None;
password(account) -> str; ensure(account) -> account; note_hero(account, char); console(line, seconds).

  wowaccount.py list                 the playthrough accounts and their hero characters (no passwords)
  wowaccount.py of CHAR              which account owns CHAR, and whether it is one of ours
  wowaccount.py new [HERO]           make a playthrough account now (tests); prints its name
  wowaccount.py password ACCOUNT     its password, for benilla's starter only (never into a log)
"""
import json, os, re, secrets, string, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowsave  # noqa: E402  (sql on the characters database, as root)
import wowserver  # noqa: E402  (the console of the right server)

ROOT = Path(__file__).resolve().parents[2]  # the development workspace; relenv.py sets it in the release
PREFIX = 'WB3P'
USER = re.compile(r'WB3P[A-Z0-9]{8}')
ALNUM = string.ascii_uppercase + string.digits


def home():
    if os.environ.get('WOWBG3_ACCOUNTS_DIR'):
        return Path(os.environ['WOWBG3_ACCOUNTS_DIR'])
    if os.environ.get('WOWBG3_DATA'):
        return Path(os.environ['WOWBG3_DATA'])
    if os.environ.get('WOWBG3_SERVER_VAR'):  # release mode's server var is DATA/server (tools run in release mode)
        return Path(os.environ['WOWBG3_SERVER_VAR']).parent
    if os.environ.get('WOW_BG3_SERVER'):
        return Path(os.environ['WOW_BG3_SERVER']) / 'var'
    return ROOT / 'server/private'


def _write_private(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    fd = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(text)
    os.chmod(str(tmp), 0o600)
    tmp.replace(path)


def load():
    try:
        reg = json.loads((home() / 'accounts.json').read_text())
    except (OSError, ValueError):
        reg = {}
    reg.setdefault('Accounts', {})
    return reg


def save(reg):
    _write_private(home() / 'accounts.json', json.dumps(reg, indent=1) + '\n')


def default_user():
    """The default account's name (for log lines only): the release's account.env, else the test account."""
    for f in (Path(os.environ.get('WOWBG3_DATA') or '/nonexistent') / 'account.env',
              ROOT / 'server/private/test-account.env'):
        try:
            for line in f.read_text().splitlines():
                if line.startswith('WOW_USER='):
                    return line.split('=', 1)[1].strip()
        except OSError:
            continue
    return 'the default account'


def _sql_user_exists(user):
    return bool(wowsave.sql("select id from realmd.account where username = '%s';" % user).split())


def account_of(char):
    """The username of the account that owns WoW character `char`, or None."""
    if not wowsave.NAME.fullmatch(char or ''):
        return None
    out = wowsave.sql("select a.username from characters c join realmd.account a on a.id = c.account "
                      "where c.name = '%s';" % char).split()
    return out[0].upper() if out else None


def console(line, seconds=3):
    """One line to the server's console (wowserver.py: A's Docker test server, a portable one). The release's
    coordinator may replace it, as it does lifecycle.console."""
    return wowserver.console(line, seconds)


def _console(line, secret):
    return console(line, 3).replace(secret, '<redacted>')


def _pw():
    return ''.join(secrets.choice(ALNUM) for _ in range(16))


def _register(user, **kw):
    reg = load()
    reg['Accounts'][user] = dict(reg['Accounts'].get(user) or {}, **kw)
    save(reg)


def _create(user, pw):
    out = _console('account create %s %s' % (user, pw), pw)
    end = time.time() + 15  # the core writes the row through its database queue
    while not _sql_user_exists(user):
        if time.time() > end:
            raise RuntimeError('account %s not made: %s' % (user, ' '.join(out.split())[-160:]))
        time.sleep(0.5)


def new(hero='', session=None):
    """A new playthrough account, made on the server and kept in the registry. Returns its name."""
    for _ in range(5):
        user = PREFIX + ''.join(secrets.choice(ALNUM) for _ in range(8))
        if user not in load()['Accounts'] and not _sql_user_exists(user):
            break
    else:
        raise RuntimeError('no free account name')
    pw = _pw()
    _register(user, Pass=pw, Made=time.time(), Hero=None, For=hero, Session=session)  # noted first: never lost
    _create(user, pw)
    return user


def password(user):
    """The playthrough account's password (registry only, no server call)."""
    acc = load()['Accounts'].get(user) or {}
    if not acc.get('Pass'):
        raise KeyError('no password for %s (wowaccount.ensure renews it)' % user)
    return acc['Pass']


def has_password(user):
    """Whether the server's row of account `user` has a password (wowsave.py rehome makes rows without one)."""
    return wowsave.sql("select ifnull(v, '') <> '' from realmd.account where username = '%s';" % user).split()[:1] == ['1']


def ensure(user):
    """The account on the server with the registry's password: made if the server lacks it (a fresh
    database), a new password through the console if the registry lost it, the registry's password set if the
    server's row has none (made by wowsave.py rehome before mangosd started). Returns user."""
    assert USER.fullmatch(user or ''), 'a playthrough account name (WB3P + 8)'
    acc = load()['Accounts'].get(user) or {}
    if not _sql_user_exists(user):
        pw = acc.get('Pass') or _pw()
        _register(user, Pass=pw, Made=acc.get('Made') or time.time())
        _create(user, pw)
    elif not acc.get('Pass') or not has_password(user):
        pw = acc.get('Pass') or _pw()
        out = _console('account set password %s %s %s' % (user, pw, pw), pw)
        if 'password' not in out.lower():
            raise RuntimeError('password of %s not set: %s' % (user, ' '.join(out.split())[-160:]))
        _register(user, Pass=pw, Reset=time.time())
    return user


def note_hero(user, char):
    if user and user in load()['Accounts']:
        _register(user, Hero=char)


def account_for(char):
    """The playthrough account CHAR logs in with (ensured: on the server, password known), or None for the
    default account (a character of an older save, or one not made yet)."""
    owner = account_of(char)
    if owner and USER.fullmatch(owner):
        return ensure(owner)
    return None


def describe(char, user):
    """A log line: which account CHAR logs in with."""
    if user:
        return 'login %s: playthrough account %s' % (char, user)
    owner = account_of(char)
    d = default_user()
    if owner and owner != d.upper():
        return 'login %s: default account %s, but it is on %s (benilla will not find it there)' % (char, d, owner)
    return 'login %s: default account %s%s' % (char, d, '' if owner else ' (no such character yet)')


def main(a):
    if a[:1] == ['list']:
        reg = load()
        print('registry %s: %d playthrough account(s)' % (home() / 'accounts.json', len(reg['Accounts'])))
        for u, v in sorted(reg['Accounts'].items(), key=lambda kv: kv[1].get('Made') or 0):
            print(u, v.get('Hero') or '-', time.strftime('%Y-%m-%d %H:%M', time.localtime(v.get('Made') or 0)))
    elif a[:1] == ['of'] and len(a) == 2:
        u = account_of(a[1])
        print(a[1], u or '-', 'playthrough' if u in load()['Accounts'] else 'other')
    elif a[:1] == ['new'] and len(a) <= 2:
        print(new(a[1] if len(a) > 1 else ''))
    elif a[:1] == ['password'] and len(a) == 2 and USER.fullmatch(a[1]):
        sys.stdout.write(password(a[1]) + '\n')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
