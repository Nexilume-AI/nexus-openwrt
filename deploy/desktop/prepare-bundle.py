"""Package an unbooted, clean desktop VHDX; never export a running router disk."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path


def prepare(image: Path, output: Path):
    image = image.absolute()
    output = output.absolute()
    for path in (image, output):
        for item in (path, *path.parents):
            if item.is_symlink() or (hasattr(item, 'is_junction') and item.is_junction()):
                raise ValueError('Linked paths are not supported')
    if image.suffix.lower() != '.vhdx' or not image.is_file():
        raise ValueError('Supply a clean VHDX produced by the desktop image recipe')
    with image.open('rb') as source:
        if source.read(8) != b'vhdxfile':
            raise ValueError('Not a VHDX file')
    output.mkdir(parents=True, exist_ok=False)
    target = output/'nexus-openwrt-desktop.vhdx'
    shutil.copyfile(image, target)
    with target.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    manifest = dict(schema_version=1, profile='nexus-openwrt-hyperv-v1', architecture='x86_64', generation=2,
                    lan_address='192.168.246.1', host_address='192.168.246.2',
                    lan_ipv6='fd6e:6578:7573:246::1', host_ipv6='fd6e:6578:7573:246::2', file=target.name, sha256=digest)
    (output/'desktop-image.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    for name in ('start-nexus-openwrt.ps1', 'configure-ipv6.ps1', 'README.md'):
        shutil.copyfile(Path(__file__).parent/name, output/name)
    print('Bundle prepared. This checksum does not certify provenance, sanitization, or boot acceptance.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.image, args.output)
