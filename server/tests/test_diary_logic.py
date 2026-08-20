"""
日记生成逻辑 单元测试 (纯函数, 本地可跑):
    cd server && python -m unittest discover -s tests -v
"""
import sys, os, unittest
from datetime import date, datetime, timezone

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.tasks.diary_tasks import diary_window_utc, _coerce_result


class TestDiaryWindowUtc(unittest.TestCase):
    def test_regular_day(self):
        s, e = diary_window_utc(date(2026, 8, 18))
        self.assertEqual(s, datetime(2026, 8, 17, 16, 0, tzinfo=timezone.utc))
        self.assertEqual(e, datetime(2026, 8, 18, 16, 0, tzinfo=timezone.utc))

    def test_month_crossing(self):
        s, _ = diary_window_utc(date(2026, 8, 1))
        self.assertEqual(s, datetime(2026, 7, 31, 16, 0, tzinfo=timezone.utc))

    def test_year_crossing(self):
        s, _ = diary_window_utc(date(2026, 1, 1))
        self.assertEqual(s, datetime(2025, 12, 31, 16, 0, tzinfo=timezone.utc))

    def test_window_is_exactly_24h(self):
        s, e = diary_window_utc(date(2026, 2, 28))
        self.assertEqual((e - s).total_seconds(), 86400)


class TestCoerceResult(unittest.TestCase):
    def test_valid_passthrough(self):
        t, c, m, fb = _coerce_result(
            {"title": "摸头的一天", "content": "今天主人摸了我的头，好开心。" * 10,
             "mood_summary": "开心"},
            "素材", date(2026, 8, 18),
        )
        self.assertEqual(t, "摸头的一天")
        self.assertEqual(m, "开心")
        self.assertFalse(fb)

    def test_raw_fallback(self):
        t, c, m, fb = _coerce_result(
            {"_raw": "不是JSON"}, "", date(2026, 8, 18),
        )
        self.assertEqual(t, "08月18日 的一天")
        self.assertEqual(c, "今天和主人一起度过了平凡的一天。")
        self.assertEqual(m, "")
        self.assertTrue(fb)

    def test_empty_fields_fallback(self):
        t, c, m, fb = _coerce_result(
            {"title": "", "content": "短"}, "", date(2026, 8, 18),
        )
        self.assertEqual(t, "08月18日 的一天")
        self.assertTrue(fb)

    def test_length_caps(self):
        t, c, m, fb = _coerce_result(
            {"title": "长" * 50, "content": "中" * 5000, "mood_summary": "开开心心"},
            "", date(2026, 8, 18),
        )
        self.assertEqual(len(t), 20)
        self.assertEqual(len(c), 600)
        self.assertEqual(len(m), 4)
        self.assertFalse(fb)


if __name__ == "__main__":
    unittest.main()
