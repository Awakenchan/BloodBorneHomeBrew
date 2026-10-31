#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import json
from pathlib import Path

import engine_assets
import mods


def settings_value(config, key):
    if not config.is_file():
        return None
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def layers(game, engine, data, config, patches):
    out = []
    own_font = all((game / 'dvdroot_ps4/font' / name).is_file() for name in ('DbgFont14h.ccm', 'DbgFont14h.tpf'))
    if settings_value(config, 'debug_menu') == '1' and not own_font and (engine / 'debug-font').is_dir():
        out.append(engine / 'debug-font')
    try:
        enabled = json.loads(patches.read_text(encoding='utf-8')).get('enabled', [])
    except (OSError, ValueError):
        enabled = []
    bosses = 'Bloodborne.xml/Lamp2Lamp: warp to bosses' in enabled
    if bosses or 'Bloodborne.xml/Lamp2Lamp (warp between lamps)' in enabled:
        try:
            for name in ('lamp2lamp', 'lamp2lamp-bosses') if bosses else ('lamp2lamp',):
                out.append(engine_assets.prepare_lamp2lamp(game, mods.update_folder(game), engine / f'{name}.bin', data / 'engine-assets'))
            print('Engine: Lamp2Lamp on', flush=True, file=__import__('sys').stderr)
        except (OSError, RuntimeError, ValueError) as error:
            print(f'Engine: Lamp2Lamp off: {error}', flush=True, file=__import__('sys').stderr)
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--patches', type=Path, required=True)
    args = parser.parse_args()
    for layer in layers(args.game.resolve(), args.engine.resolve(), args.data, args.config, args.patches):
        print(layer)
