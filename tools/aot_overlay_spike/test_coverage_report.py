#!/usr/bin/env python3
import os
import tempfile
import unittest
import zlib

import coverage_report


class CoverageReportTests(unittest.TestCase):
    def test_bios_parser_is_scoped_to_named_tables(self):
        source = r'''
static const DispatchEntry dispatch_table[2] = {
    { 0x00000500u, func_00000500 },
    { 0xBFC00180u, func_BFC00180 },
};
static const unsigned unrelated[][3] = {
    { 0xDEADBEEFu, 0x00000001u, 0x1FFFFFFFu },
};
const PsxKernelBody psx_bios_kernel_bodies[2] = {
    { 0x00000500u, 0x00000500u, 0x00000510u },
    { 0x00000510u, 0x00000510u, 0x00000520u },
};
const PsxNativeStub psx_bios_native_stubs[1] = {
    { 0x000000A0u, 0x000000A0u, 0x000000B0u },
};
'''
        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.c', delete=False, encoding='utf-8') as generated:
            generated.write(source)
            path = generated.name
        try:
            entries, ranges = coverage_report.parse_bios_dispatch(path)
        finally:
            os.unlink(path)
        self.assertEqual(entries, {0xA0, 0x500, 0x1FC00180})
        self.assertEqual(ranges, [(0xA0, 0xB0), (0x500, 0x520)])

    def test_code_range_recall_is_distinct_from_exact_entry_recall(self):
        result = coverage_report.recall(
            {0x100, 0x104}, {0x100: 0x1234}, [(0x100, 0x108)])
        self.assertEqual(result['covered_here'], 1)
        self.assertEqual(result['covered_by_code_range'], 2)

    def test_all_zero_compiled_range_is_quarantined(self):
        with tempfile.TemporaryDirectory() as tmp:
            zero_size = 0x184
            zero_crc = zlib.crc32(bytes(zero_size)) & 0xFFFFFFFF
            with open(os.path.join(tmp, 'mixed.ranges'), 'w', encoding='utf-8') as out:
                out.write('# psxrecomp overlay code-range manifest v2\n')
                out.write('F 8000EE7C %08X\n' % zero_crc)
                out.write('R 8000EE7C %X\n' % zero_size)
                out.write('F 80000100 12345678\n')
                out.write('R 80000100 10\n')
            entries, ranges, audit = coverage_report.parse_ranges_dir(
                tmp, with_audit=True)
        self.assertEqual(entries, {0x100: 0x12345678})
        self.assertEqual(ranges, [(0x100, 0x110)])
        self.assertEqual(audit, [{
            'entry': '0x8000EE7C', 'code_crc': '%08X' % zero_crc,
            'size': zero_size}])


if __name__ == '__main__':
    unittest.main()
