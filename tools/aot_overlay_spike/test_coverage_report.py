#!/usr/bin/env python3
import os
import tempfile
import unittest

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
'''
        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.c', delete=False, encoding='utf-8') as generated:
            generated.write(source)
            path = generated.name
        try:
            entries, ranges = coverage_report.parse_bios_dispatch(path)
        finally:
            os.unlink(path)
        self.assertEqual(entries, {0x500, 0x1FC00180})
        self.assertEqual(ranges, [(0x500, 0x520)])

    def test_code_range_recall_is_distinct_from_exact_entry_recall(self):
        result = coverage_report.recall(
            {0x100, 0x104}, {0x100: 0x1234}, [(0x100, 0x108)])
        self.assertEqual(result['covered_here'], 1)
        self.assertEqual(result['covered_by_code_range'], 2)


if __name__ == '__main__':
    unittest.main()
