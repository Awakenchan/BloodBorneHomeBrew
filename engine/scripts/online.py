# SPDX-License-Identifier: GPL-2.0-or-later
"""The single INI online profile and automatically generated game host override."""
import json
import os
from pathlib import Path
from urllib.parse import urlsplit

GAME_STATUS_ORIGIN = 'https://ss4.scej-network.jp:20443'
# Messages, bloodstains and ghosts: Bloodborne's own game server. shadNet servers only sign in and
# match players (co-op); the public one has no game server, so The Hunter's Dream is the default.
HUNTERS_DREAM = 'https://thehuntersdream.com'
# The game PUTs its play logs to FromSoftware's S3 buckets (bucket names nobody here owns); the game
# server takes them on its API port instead (PUT /<date>/<file>, plain HTTP), as bbhost sends them
# (droogie/bbhost src/hle/http.cpp).
PLAY_LOG_HOSTS = ('bb-playlog-prod.s3.amazonaws.com', 'bb-playlog-test.s3.amazonaws.com')
GAME_API_PORT = 18671
SHADNET_HOST = 'srv.shadps4.net'
SHADNET_PORT = 31313
SHADNET_WEBAPI = f'http://{SHADNET_HOST}:31315'
# P2P (co-op) port when none is set: the PS4's own, so UPnP and firewall rules stay the same
# between games.
DEFAULT_P2P_PORT = 3658
# Seconds a summon sign entry waits for its host (the game's 180; engine/online_fixes.cpp).
DEFAULT_SIGN_TIMEOUT = 30
THD_ENV = ('BB_NET_BACKEND', 'BB_THD_SERVER', 'BB_THD_AUTH', 'BB_THD_TOKEN', 'BB_THD_ONLINE_ID',
           'BB_THD_RULESET')
ONLINE_ENV = ('BB_ONLINE', 'BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID',
              'BB_SHADNET_PASSWORD', 'BB_SHADNET_TOKEN', 'BB_UPNP', 'BB_P2P_PORT',
              'SHADPS4_HTTP_HOST_OVERRIDES_JSON', 'BB_SIGN_TIMEOUT') + THD_ENV

def read_settings(path):
    settings = {}
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        if line.lstrip().startswith(('#', ';')):
            continue
        key, sep, value = line.partition('=')
        if sep:
            settings[key.strip()] = value.strip()
    return settings

def game_server(text):
    """The game server as typed: empty is The Hunter's Dream, a bare name gets https://. A
    server without a port keeps the game's own (20443)."""
    text = (text or '').strip()
    if not text:
        return HUNTERS_DREAM
    if '://' not in text:
        text = 'https://' + text
    # Earlier versions defaulted to the shadNet Web API, which has no game server.
    if urlsplit(text).hostname == SHADNET_HOST:
        return HUNTERS_DREAM
    return text

def http_base(text):
    if not text or any(ord(c) < 32 for c in text):
        raise ValueError('Online requires an explicit WebAPI / game API URL')
    value = urlsplit(text)
    if value.scheme not in ('http', 'https') or not value.hostname or value.username is not None or value.password is not None or value.query or value.fragment:
        raise ValueError('Online API must be an HTTP(S) URL without credentials, query or fragment')
    if value.port is not None and not 1 <= value.port <= 65535:
        raise ValueError('Invalid online API port')
    return text.rstrip('/')

def game_overrides(game_api):
    overrides = {GAME_STATUS_ORIGIN: game_api}
    overrides.update({h: f'{urlsplit(game_api).scheme}://{urlsplit(game_api).hostname}:{GAME_API_PORT}' for h in PLAY_LOG_HOSTS})
    return overrides

# BB_ONLINE_PAUSED=1 keeps the game offline whatever lantern.ini says.
ONLINE_PAUSED = os.environ.get('BB_ONLINE_PAUSED', '0') == '1'


def configure_online(settings, data, env):
    # The Hunter's Dream pool is the reworked connection: the pause is the shadNet pool's.
    if (settings.get('online_pool') == 'huntersdream' and settings.get('online_enabled') == '1' and
            settings.get('online_connected', '1') == '1'):
        return configure_huntersdream(settings, data, env)
    if ONLINE_PAUSED and settings.get('online_enabled') == '1':
        print("Online: paused in this version while we rework the connection together with the shadNet "
              "and The Hunter's Dream teams; the game starts offline", flush=True)
    if (ONLINE_PAUSED or settings.get('online_enabled') != '1' or
            settings.get('online_connected', '1') != '1'):
        for key in ONLINE_ENV:
            env.pop(key, None)
        env['BB_ONLINE'] = '0'
        return None
    for key in THD_ENV:
        env.pop(key, None)
    host = settings.get('online_host', '').strip().strip('[]') or SHADNET_HOST
    if any(c.isspace() or ord(c) < 32 for c in host) or any(c in host for c in '/@?#:'):
        raise ValueError('Enter a shadNet server name without a scheme, path or port')
    port = int(settings.get('online_port', '') or SHADNET_PORT)
    if not 1 <= port <= 65535:
        raise ValueError('shadNet TCP port must be between 1 and 65535')
    default_webapi = SHADNET_WEBAPI if host == SHADNET_HOST else f'http://{host}:31315'
    webapi = http_base(settings.get('online_webapi', '').strip() or default_webapi)
    game_api = http_base(game_server(settings.get('online_game_api', '')))
    if urlsplit(webapi).path not in ('', '/') or urlsplit(game_api).path not in ('', '/'):
        raise ValueError('WebAPI and game API require a server origin without an extra path')
    username = settings.get('online_username', '')
    if not 1 <= len(username.encode('utf-8')) <= 16 or any(c.isspace() or ord(c) < 32 for c in username):
        raise ValueError('Enter the shadNet account name (up to 16 characters)')
    if not env.get('BB_SHADNET_PASSWORD'):
        raise ValueError('Sign in with the account password in the launcher')
    p2p = settings.get('online_p2p_port', '').strip()
    if p2p and not 1 <= int(p2p) <= 65535:
        raise ValueError('Local P2P port must be between 1 and 65535, or empty for the default (3658)')
    # Runtime cache only. Never read or overwrite another application's JSON,
    # and never route private-server traffic through a public server by default.
    destination = Path(data) / 'network/host_overrides.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(game_overrides(game_api), indent=2), encoding='utf-8')
    os.replace(temporary, destination)
    env.update(BB_ONLINE='1', BB_SHADNET_SERVER=(f'[{host}]' if ':' in host else host) + f':{port}',
               BB_SHADNET_WEBAPI=webapi, BB_SHADNET_NPID=username,
               BB_UPNP='0' if settings.get('online_upnp') == '0' else '1',
               BB_P2P_PORT=str(int(p2p)) if p2p else str(DEFAULT_P2P_PORT),
               SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(destination.resolve()))
    timeout = settings.get('online_sign_timeout', '').strip()
    env['BB_SIGN_TIMEOUT'] = str(min(max(int(timeout), 0), 600)) if timeout.isdigit() else str(DEFAULT_SIGN_TIMEOUT)
    return destination


def p2p_port(settings):
    p2p = settings.get('online_p2p_port', '').strip()
    if p2p and not 1 <= int(p2p) <= 65535:
        raise ValueError('Local P2P port must be between 1 and 65535, or empty for the default (3658)')
    return str(int(p2p)) if p2p else str(DEFAULT_P2P_PORT)


def write_overrides(data, overrides):
    destination = Path(data) / 'network/host_overrides.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(overrides, indent=2), encoding='utf-8')
    os.replace(temporary, destination)
    return destination


def configure_huntersdream(settings, data, env):
    """The Hunter's Dream pool: the engine's own backend for it (BB_NET_BACKEND=thd), bbhost's
    protocol. The game's servers (*.scej-network.jp, its own ports) and play logs (the API port)
    go to the game server; the account comes from the launcher's Discord sign-in."""
    game_api = http_base(game_server(settings.get('online_game_api', '')))
    origin = urlsplit(game_api)
    if origin.path not in ('', '/'):
        raise ValueError('The game server needs an origin without a path')
    if not env.get('BB_THD_TOKEN') or not env.get('BB_THD_ONLINE_ID'):
        raise ValueError("Sign in to The Hunter's Dream with Discord on the launcher's Online page")
    scheme, host = origin.scheme, origin.hostname
    base = f'{scheme}://{host}'
    overrides = {'*.scej-network.jp': base}
    overrides.update({h: f'{base}:{GAME_API_PORT}' for h in PLAY_LOG_HOSTS})
    destination = write_overrides(data, overrides)
    for key in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD',
                'BB_SHADNET_TOKEN', 'BB_UPNP'):
        env.pop(key, None)
    env.update(BB_ONLINE='1', BB_NET_BACKEND='thd', BB_THD_SERVER=f'{base}:{GAME_API_PORT}',
               BB_THD_AUTH=base, BB_THD_RULESET=settings.get('online_thd_ruleset', '').strip() or 'vanilla',
               BB_P2P_PORT=p2p_port(settings), SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(destination.resolve()))
    timeout = settings.get('online_sign_timeout', '').strip()
    env['BB_SIGN_TIMEOUT'] = str(min(max(int(timeout), 0), 600)) if timeout.isdigit() else str(DEFAULT_SIGN_TIMEOUT)
    return destination


def main(argv):
    import argparse
    import sys
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    parser.add_argument('--data', required=True)
    args = parser.parse_args(argv)
    config = Path(args.config)
    env = dict(os.environ)
    try:
        override = configure_online(read_settings(config) if config.is_file() else {}, args.data, env)
    except (OSError, ValueError) as error:
        print(f'Online configuration: {error}', file=sys.stderr)
        return 1
    print('Online: configured server profile and automatic game redirect' if override else 'Online: offline mode',
          file=sys.stderr)
    for key in sorted(set(os.environ) - set(env)):
        print(f'unset {key}')
    for key, value in sorted(env.items()):
        if os.environ.get(key) != value and '\n' not in value:
            print(f'{key}={value}')
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv[1:]))
