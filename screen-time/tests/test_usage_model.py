import unittest

from usage_model import (
    categorize_program, categorize_legacy_key, firestore_day, migrate_legacy_day,
    normalized_day, should_count,
)


class UsageModelTests(unittest.TestCase):
    def test_chrome(self):
        self.assertEqual(categorize_program('chrome.exe', 'Vídeo 1 - YouTube - Google Chrome'), ('tv', 'youtube'))
        self.assertEqual(categorize_program('chrome.exe', 'Vídeo 2 - YouTube - Google Chrome'), ('tv', 'youtube'))
        self.assertEqual(categorize_program('chrome.exe', 'Meu filme - Netflix - Google Chrome'), ('tv', 'netflix'))
        self.assertEqual(categorize_program('chrome.exe', 'ChatGPT - Google Chrome'), ('pc', 'chatgpt'))
        self.assertEqual(categorize_program('chrome.exe', 'Gmail - Google Chrome'), ('pc', 'chromeOutros'))

    def test_executables(self):
        for exe, target in (
            ('mpc-hc64.exe', ('tv', 'mpc')),
            ('code.exe', ('pc', 'vscode')),
            ('photoshop.exe', ('pc', 'photoshop')),
            ('explorer.exe', ('pc', 'explorer')),
            ('amazon music.exe', ('pc', 'amazonMusic')),
            ('whatsapp.root.exe', ('pc', 'whatsapp')),
            ('thing.exe', ('pc', 'thing.exe')),
        ):
            with self.subTest(exe=exe):
                self.assertEqual(categorize_program(exe), target)

    def test_legacy_and_sums(self):
        legacy = {'chrome.exe': 67, 'code.exe': 232, 'explorer.exe': 1,
                  'chrome: YouTube': 100, 'mpc-hc64.exe': 50}
        upgraded = migrate_legacy_day(legacy)
        summary = firestore_day(upgraded)
        self.assertEqual(summary['tv']['totalSeconds'], 150)
        self.assertEqual(summary['pc']['totalSeconds'], 300)
        self.assertEqual(summary['pc']['chromeOutros'], 67)
        self.assertEqual(categorize_legacy_key('chrome: Meu título - Netflix'), ('tv', 'netflix'))

    def test_totals_use_rounded_categories(self):
        summary = firestore_day({'tv': {'youtube': 12.6, 'mpc': 5.7}, 'pc': {'vscode': 10.5}})
        self.assertEqual(summary['tv']['totalSeconds'], sum(v for k, v in summary['tv'].items() if k != 'totalSeconds'))
        self.assertEqual(summary['pc']['totalSeconds'], sum(v for k, v in summary['pc'].items() if k != 'totalSeconds'))

    def test_pc_activity_and_tv_without_activity(self):
        self.assertTrue(should_count(('pc', 'vscode'), 0, 60))
        self.assertTrue(should_count(('pc', 'vscode'), 59.9, 60))
        self.assertFalse(should_count(('pc', 'vscode'), 60, 60))
        self.assertTrue(should_count(('tv', 'youtube'), 100000, 60))
        self.assertTrue(should_count(('tv', 'mpc'), 100000, 60))
        self.assertFalse(should_count(None, 0, 60))

    def test_unlisted_apps_get_their_own_fields_and_survive_normalization(self):
        self.assertEqual(categorize_program('Notepad.EXE'), ('pc', 'notepad.exe'))
        self.assertEqual(categorize_program('Discord.exe'), ('pc', 'discord.exe'))
        raw = {'tv': {'youtube': 200},
               'pc': {'notepad.exe': 15.6, 'discord.exe': 21, 'outrosProgramas': 99}}
        reloaded = normalized_day(raw)
        self.assertEqual(reloaded['pc']['notepad.exe'], 15.6)
        summary = firestore_day(reloaded)
        self.assertEqual(summary['pc']['notepad.exe'], 16)
        self.assertEqual(summary['pc']['discord.exe'], 21)
        # Dado anterior agrupado não pode ser separado retroativamente.
        self.assertEqual(summary['pc']['outrosProgramas'], 99)
        self.assertEqual(summary['pc']['totalSeconds'], 136)


if __name__ == '__main__':
    unittest.main()
