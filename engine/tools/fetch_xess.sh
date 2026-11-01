#!/usr/bin/env bash
# Downloads Intel's XeSS Super Resolution library for upscaler=xess (docs/xess.md, vk_xess.cpp)
# from Intel's release of the XeSS SDK (github.com/intel/xess): libxess.dll (about 75 MB) with the
# SDK's LICENSE.txt (Intel Simplified Software License: redistribution in binary form, unchanged,
# with the notice) and third-party-programs.txt. They are not part of this repository. The engine
# loads them from engine/xess (next to out/) or BB_XESS_DIR. Windows x64 only: Intel ships no
# Linux library. libxess.dll needs the Microsoft Visual C++ 2015-2022 runtime on the player's PC.
# XeSS Frame Generation (libxess_fg.dll) and XeLL are D3D12 only and not downloaded.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=3.0.2
sha256=88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990
dest=${1:-xess}
mkdir -p "$dest"
if [[ -s $dest/libxess.dll && -s $dest/LICENSE-XeSS.txt && -s $dest/VERSION.txt &&
      $(<"$dest/VERSION.txt") == "$version" ]]; then
    echo "XeSS: $dest has libxess.dll $version"
    exit 0
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
curl -fSL --retry 3 -o "$work/sdk.zip" \
    "https://github.com/intel/xess/releases/download/v$version/XeSS_SDK_$version.zip"
# python3, or python where python3 is only the Windows Store stub.
py=python3
"$py" -c '' 2>/dev/null || py=python
"$py" - "$work/sdk.zip" "$dest" "$version" "$sha256" <<'EOF'
import hashlib, os, sys, zipfile
archive, dest, version, sha256 = sys.argv[1:]
with open(archive, 'rb') as f:
    digest = hashlib.sha256(f.read()).hexdigest()
if digest != sha256:
    sys.exit(f'XeSS: {archive} has SHA-256 {digest}, expected {sha256}')
files = {'bin/libxess.dll': 'libxess.dll', 'LICENSE.txt': 'LICENSE-XeSS.txt',
         'third-party-programs.txt': 'third-party-programs-XeSS.txt'}
with zipfile.ZipFile(archive) as z:
    for name, target in files.items():
        part = os.path.join(dest, target + '.part')
        with open(part, 'wb') as f:
            f.write(z.read(name))
        os.replace(part, os.path.join(dest, target))
with open(os.path.join(dest, 'VERSION.txt'), 'w') as f:
    f.write(version)
EOF
echo "XeSS: SDK $version libxess.dll in $dest"
