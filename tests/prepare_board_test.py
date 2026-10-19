"""Inspection permission/order/failure tests: mocked programmer only."""
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
import io
import sys
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_board as tool

class PrepareTests(unittest.TestCase):
    def test_no_target_access_without_explicit_connect(self):
        for action in ('inspect', 'backup'):
            with patch.object(tool.flash, 'Programmer') as constructor, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit): tool.main([action])
                constructor.assert_not_called()
    def test_listing_does_not_select_or_connect(self):
        programmer = Mock()
        with patch.object(tool.flash, 'locate_programmer'), patch.object(tool.flash, 'Programmer', return_value=programmer), redirect_stdout(io.StringIO()):
            self.assertEqual(tool.main(['list']), 0)
        programmer.run.assert_called_once_with(['-l', 'st-link-only'])
        programmer.select.assert_not_called()
    def test_inspect_only_displays_option_bytes(self):
        programmer = Mock()
        programmer.connection.return_value = ['-c', 'port=SWD', 'sn=MOCK', 'mode=NORMAL', 'freq=1000']
        with patch.object(tool.flash, 'locate_programmer'), patch.object(tool.flash, 'Programmer', return_value=programmer), patch.object(tool.flash, 'probe_lock'), patch.object(tool.flash, 'backup') as backup, redirect_stdout(io.StringIO()):
            self.assertEqual(tool.main(['inspect', '--allow-connect']), 0)
            backup.assert_not_called()
        self.assertEqual(programmer.run.call_args.args[0][-2:], ['-ob', 'displ'])
        programmer.inspect.assert_called_once()
    def test_failed_identity_never_reads_backup_or_option_bytes(self):
        programmer = Mock()
        programmer.inspect.side_effect = tool.flash.Failure('wrong MCU')
        with patch.object(tool.flash, 'locate_programmer'), patch.object(tool.flash, 'Programmer', return_value=programmer), patch.object(tool.flash, 'probe_lock'), patch.object(tool.flash, 'backup') as backup:
            with self.assertRaises(tool.flash.Failure): tool.main(['backup', '--allow-connect'])
            backup.assert_not_called()
            programmer.run.assert_not_called()

if __name__ == '__main__': unittest.main()
