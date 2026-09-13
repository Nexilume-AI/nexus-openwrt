"""Exercise clock selection and malformed samples without changing system time."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipIf(os.name == 'nt', 'POSIX shell/device fixture')
class TimeSyncTests(unittest.TestCase):
    def run_service(self, names, sample):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'ptp').mkdir()
            (root/'dev').mkdir()
            for i, name in enumerate(names):
                clock = root/'ptp'/f'ptp{i}'
                clock.mkdir()
                (clock/'clock_name').write_text(name+'\n')
                (root/'dev'/f'ptp{i}').symlink_to('/dev/null')
            source = (Path(__file__).parent/'files/etc/init.d/nexus-desktop-time').read_text()
            source = source.replace('/sys/class/ptp/ptp*', str(root/'ptp/ptp*'))
            source = source.replace('clock="/dev/', 'clock="'+str(root/'dev')+'/')
            # The fake hardware reader records a sink write; it never sets a clock.
            script = source + '''
logger() { :; }
phc_ctl() {
    if [ "$2" = CLOCK_REALTIME ]; then
        printf '%s|%s|%s\n' "$2" "$3" "$4" > "$RECORD"
    else
        printf '%s\n' "$SAMPLE"
    fi
}
start
'''
            record = root/'record'
            result = subprocess.run(['sh'], input=script, text=True, capture_output=True,
                env={**os.environ, 'RECORD':str(record), 'SAMPLE':sample})
            return result.returncode, record.read_text() if record.exists() else ''

    def test_valid_hyperv_sets_only_system_clock(self):
        code, write = self.run_service(['unrelated', 'hyperv'], 'clock time is 1789273429.123 or date')
        self.assertEqual(code, 0)
        self.assertEqual(write, 'CLOCK_REALTIME|set|1789273429.123\n')

    def test_invalid_sample_does_not_set_clock(self):
        for sample in ('error', 'clock time is 1.2 or date', 'clock time is 1789273429.123 or date\nclock time is 1789273430.123 or date'):
            with self.subTest(sample=sample):
                code, write = self.run_service(['hyperv'], sample)
                self.assertNotEqual(code, 0)
                self.assertEqual(write, '')

    def test_missing_or_ambiguous_hyperv_clock_is_refused(self):
        for names in ([], ['unrelated'], ['hyperv', 'hyperv']):
            with self.subTest(names=names):
                code, write = self.run_service(names, 'clock time is 1789273429.123 or date')
                self.assertNotEqual(code, 0)
                self.assertEqual(write, '')
