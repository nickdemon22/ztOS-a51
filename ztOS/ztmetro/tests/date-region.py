"""Validate Settings requests without modifying the system clock or locale."""
from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]
backend = runpy.run_path(str(ROOT / 'os/overlay/usr/local/libexec/ztos-date-region'))
validate = backend['validate']
validate_servers = backend['validate_servers']

class DateRegionTests(unittest.TestCase):
    def test_valid_requests(self):
        validate('UTC', 'en_US.UTF-8', 'yes', '')
        validate('Asia/Colombo', 'si_LK.UTF-8', 'no', '2028-02-29 23:59')

    def test_ntp_servers(self):
        self.assertEqual(validate_servers('lk.pool.ntp.org 192.0.2.1 ::1'),
                         'lk.pool.ntp.org 192.0.2.1 ::1')
        for value in ('', 'pool.ntp.org;id', 'host\nNTP=attacker', '-invalid.host', 'a b c d e'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_servers(value)

    def test_invalid_requests(self):
        for values in [
            ('../../etc/passwd', 'en_US.UTF-8', 'yes', ''),
            ('UTC;id', 'en_US.UTF-8', 'yes', ''),
            ('not/a/zone', 'en_US.UTF-8', 'yes', ''),
            ('UTC', 'anything', 'yes', ''),
            ('UTC', 'en_US.UTF-8', 'maybe', ''),
            ('UTC', 'en_US.UTF-8', 'no', '2026-02-29 12:00'),
            ('UTC', 'en_US.UTF-8', 'no', '2026-10-16 24:00'),
            ('UTC', 'en_US.UTF-8', 'no', '2026-1-1 1:00'),
        ]:
            with self.subTest(values=values), self.assertRaises(ValueError):
                validate(*values)

if __name__ == '__main__':
    unittest.main()
