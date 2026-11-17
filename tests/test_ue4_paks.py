"""The game's paks in both regions' formats: version 7 (North America, CUSA11227) and version 3
(Europe, CUSA12982), read for the override pak's ini and assets, and the override pak written in the
version the game's own paks use."""
import hashlib
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
import ue4_override  # noqa: E402


def compressed_pak(path, version, name, data):
    """A pak with one zlib entry in two blocks, laid out as UnrealPak writes that version."""
    half = len(data) // 2
    blocks = [zlib.compress(data[:half]), zlib.compress(data[half:])]
    header_size = 8 * 3 + 4 + 20 + 4 + 16 * len(blocks) + 1 + 4

    def entry(offset):
        out = struct.pack('<qqqi', offset, sum(map(len, blocks)), len(data), 1) + hashlib.sha1(data).digest()
        out += struct.pack('<i', len(blocks))
        start = header_size if version >= 5 else offset + header_size   # relative from version 5
        for block in blocks:
            out += struct.pack('<qq', start, start + len(block))
            start += len(block)
        return out + b'\0' + struct.pack('<I', 65536)
    body = entry(0) + b''.join(blocks)
    index = ue4_override.fstring('../../../') + struct.pack('<i', 1) + ue4_override.fstring(name) + entry(0)
    footer = struct.pack('<IIQQ', ue4_override.PAK_MAGIC, version, len(body), len(index)) + hashlib.sha1(index).digest()
    if version >= 4:
        footer = b'\0' + footer
    if version >= 7:
        footer = bytes(16) + footer
    Path(path).write_bytes(body + index + footer)


class PakVersionTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def test_written_paks_read_back_in_each_version(self):
        files = {'OVERBITE/Config/DefaultEngine.ini': b'[/Script/Engine.RendererSettings]\nr.X=1\n',
                 'OVERBITE/Content/a.uasset': bytes(range(256)) * 3}
        for version in (3, 4, 5, 6, 7):
            pak = self.dir / f'v{version}.pak'
            ue4_override.write_pak(pak, files, version=version)
            self.assertEqual(ue4_override.pak_info(pak)[0], version)
            entries = ue4_override.read_index(pak)
            for name, data in files.items():
                self.assertEqual(ue4_override.read_entry(pak, entries[name]), data)

    def test_versions_the_writer_does_not_know_become_7(self):
        pak = self.dir / 'new.pak'
        ue4_override.write_pak(pak, {'a': b'x'}, version=11)
        self.assertEqual(ue4_override.pak_info(pak)[0], 7)

    def test_compressed_entries_old_absolute_and_new_relative_blocks(self):
        data = b'MediEvil ' * 5000
        for version in (3, 7):
            pak = self.dir / f'c{version}.pak'
            compressed_pak(pak, version, 'OVERBITE/Content/big.uasset', data)
            entries = ue4_override.read_index(pak)
            self.assertEqual(ue4_override.read_entry(pak, entries['OVERBITE/Content/big.uasset']), data)

    def test_the_game_pak_version_decides_the_override_version(self):
        paks = ue4_override.paks_dir(self.dir)
        paks.mkdir(parents=True)
        ue4_override.write_pak(paks / 'pakchunk0-ps4.pak', {'a': b'x'}, version=3)
        self.assertEqual(ue4_override.game_pak_version(self.dir), 3)
        self.assertEqual(ue4_override.read_file(self.dir, 'a'), b'x')

    def test_unknown_format_says_so(self):
        bad = self.dir / 'bad.pak'
        bad.write_bytes(bytes(300))
        with self.assertRaisesRegex(ValueError, 'unsupported pak'):
            ue4_override.pak_info(bad)


if __name__ == '__main__':
    unittest.main()
